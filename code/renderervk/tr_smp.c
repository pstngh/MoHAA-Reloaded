/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// tr_smp.c -- the render thread (r_smp 1)
//
// Quake 3's SMP mode, back: the front end (the game's thread) fills a
// render command list and the per-frame data (backEndData: draw surfaces,
// entities, lights, polys) for a frame while the render thread runs the
// back end of the frame before (RB_ExecuteRenderCommands: the draws, their
// recording into Vulkan command buffers, the submission and the present).
// backEndData is doubled, the thread reads one while the front end writes
// the other; a frame is handed over at RE_EndFrame once the thread is done
// with the frame before, so at most one frame is in the thread. A frame's
// input to its present takes as long as before (the two halves still run
// one after the other for that frame), the frames come as fast as the
// longer half.
//
// Everything that touches the back end's state (tess, backEnd, the Vulkan
// frame) from the front end goes through R_FrontEndSync first: it waits for
// the thread to be idle, after which the front end owns the back end until
// it hands over the next list (registration, screenshots, the debug
// drawing, cinematics). The 2D exports, called every frame, don't: they
// record commands the thread replays (tr_draw.c, tr_font.cpp).

#include "tr_local.h"

#include <SDL3/SDL.h>

cvar_t	*r_smp;

static struct {
	SDL_Thread		*thread;
	SDL_ThreadID	threadId;
	SDL_Mutex		*lock;
	SDL_Condition	*wake;			// the thread waits for a list or quit
	SDL_Condition	*done;			// the front end waits for the thread to be idle
	const void		*pending;		// the list handed over, not taken yet
	qboolean		busy;			// a list taken, not finished
	qboolean		quit;
	int				backEndMsec;	// of the last list finished
	int				backEndUs;		// the thread's work on the last list, its waits left out

	// the front end's wait before the next frame's input (R_IssueFrameThreaded)
	uint64_t		frameStartUs;	// when the front end started the frame (after the wait)
	float			frontUs, backUs, gpuUs;	// averages of the front end's, the thread's and the GPU's time a frame
} smp;

static uint64_t R_SmpMicroseconds( void ) {
	return SDL_GetTicksNS() / 1000;
}

qboolean R_OnRenderThread( void ) {
	return smp.thread && SDL_GetCurrentThreadID() == smp.threadId;
}

static int SDLCALL R_RenderThreadMain( void *data ) {
	(void)data;

	SDL_LockMutex( smp.lock );
	for ( ;; ) {
		const void *cmds;

		while ( !smp.pending && !smp.quit ) {
			SDL_WaitCondition( smp.wake, smp.lock );
		}
		if ( smp.quit && !smp.pending ) {
			break;
		}
		uint64_t start, waits;

		cmds = smp.pending;
		smp.pending = NULL;
		smp.busy = qtrue;
		SDL_UnlockMutex( smp.lock );

		start = R_SmpMicroseconds();
		waits = vk_back_end_wait_us();
		RB_ExecuteRenderCommands( cmds );
		waits = vk_back_end_wait_us() - waits;
		start = R_SmpMicroseconds() - start;

		SDL_LockMutex( smp.lock );
		smp.busy = qfalse;
		smp.backEndMsec = backEnd.pc.msec;
		smp.backEndUs = start > waits ? (int)( start - waits ) : 0;
		SDL_BroadcastCondition( smp.done );
	}
	SDL_UnlockMutex( smp.lock );
	return 0;
}

/*
===============
R_SyncRenderThread

Waits until the thread has run every list handed over; the front end then
owns the back end's state until the next hand-over
===============
*/
void R_SyncRenderThread( void ) {
	if ( !smp.thread || R_OnRenderThread() ) {
		return;
	}
	SDL_LockMutex( smp.lock );
	while ( smp.pending || smp.busy ) {
		SDL_WaitCondition( smp.done, smp.lock );
	}
	SDL_UnlockMutex( smp.lock );
}

/*
===============
R_FrontEndSync

Before the front end touches the back end's state: nothing without the
thread, or on the thread itself
===============
*/
void R_FrontEndSync( void ) {
	if ( glConfig.smpActive ) {
		R_SyncRenderThread();
	}
}

/*
===============
R_IssueRenderCommandsThreaded

Hands a command list to the thread, once it is done with the one before
===============
*/
void R_IssueRenderCommandsThreaded( const void *cmds ) {
	R_SyncRenderThread();
	SDL_LockMutex( smp.lock );
	smp.pending = cmds;
	SDL_SignalCondition( smp.wake );
	SDL_UnlockMutex( smp.lock );
}

/*
===============
R_IssueFrameThreaded

The frame's list (RE_EndFrame): handed over as any list, then what the next
frame's input should come after is waited for here, as the back end waits
for it without the thread before the game reads its input:

- with vsync, the thread's frame: its present waits for the display (and
  until it is shown, vk.c, VK_WaitForPresentShown), so the input is read
  then. RE_BeginFrame waited for it before, after the input was read.
- where the GPU is the slowest (its time a frame above the
  front end's and the thread's together, with a margin), the GPU's frame
  before the one handed over: the slot the frame after reuses, which the
  thread would wait for at its first command. The next input is read that
  much later and its frame shown as soon (about a frame of input lag less);
  where the CPU limits the fence has signaled already, and where the two
  are even the wait would cost frame rate (the front end and the thread no
  longer overlap), so it isn't made.
===============
*/
void R_IssueFrameThreaded( const void *cmds ) {
	uint64_t now = R_SmpMicroseconds();
	int front = smp.frameStartUs && now > smp.frameStartUs ? (int)( now - smp.frameStartUs ) : 0;
	int gpu;
	qboolean wait;

	R_SyncRenderThread();
	// the thread is idle: its times and the fence are read now
	gpu = vk_last_gpu_frame_us();
	vk_prepare_input_wait();
	if ( front > 0 ) {
		smp.frontUs += ( front - smp.frontUs ) * 0.1f;
	}
	smp.backUs += ( smp.backEndUs - smp.backUs ) * 0.1f;
	if ( gpu > 0 ) {
		smp.gpuUs += ( gpu - smp.gpuUs ) * 0.1f;
	}

	SDL_LockMutex( smp.lock );
	smp.pending = cmds;
	SDL_SignalCondition( smp.wake );
	SDL_UnlockMutex( smp.lock );

	if ( vk_present_waits() ) {
		R_SyncRenderThread();
	} else {
		wait = smp.gpuUs > ( smp.frontUs + smp.backUs ) * 1.15f + 20.0f;
		if ( wait ) {
			vk_input_wait();
		}
	}
	smp.frameStartUs = R_SmpMicroseconds();
}

int R_RenderThreadBackEndMsec( void ) {
	return smp.backEndMsec;
}

/*
===============
R_StartRenderThread

At the end of R_Init, with r_smp; glConfig.smpActive says whether it runs
===============
*/
void R_StartRenderThread( void ) {
	glConfig.smpActive = qfalse;
	if ( !r_smp->integer ) {
		return;
	}
	if ( SDL_GetNumLogicalCPUCores() < 2 ) {
		ri.Printf( PRINT_ALL, "r_smp: one core, no render thread\n" );
		return;
	}
	Com_Memset( &smp, 0, sizeof( smp ) );
	smp.lock = SDL_CreateMutex();
	smp.wake = SDL_CreateCondition();
	smp.done = SDL_CreateCondition();
	if ( smp.lock && smp.wake && smp.done ) {
		smp.thread = SDL_CreateThread( R_RenderThreadMain, "render", NULL );
		if ( smp.thread ) {
			smp.threadId = SDL_GetThreadID( smp.thread );
		}
	}
	if ( !smp.thread ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: r_smp: no render thread (%s)\n", SDL_GetError() );
		R_StopRenderThread();
		return;
	}
	glConfig.smpActive = qtrue;
	ri.Printf( PRINT_ALL, "r_smp: the back end runs on a render thread\n" );
}

/*
===============
R_StopRenderThread

At RE_Shutdown, after the last list; the lists handed over are run first
===============
*/
void R_StopRenderThread( void ) {
	if ( smp.thread ) {
		SDL_LockMutex( smp.lock );
		smp.quit = qtrue;
		SDL_SignalCondition( smp.wake );
		SDL_UnlockMutex( smp.lock );
		SDL_WaitThread( smp.thread, NULL );
		smp.thread = NULL;
	}
	if ( smp.lock ) {
		SDL_DestroyMutex( smp.lock );
	}
	if ( smp.wake ) {
		SDL_DestroyCondition( smp.wake );
	}
	if ( smp.done ) {
		SDL_DestroyCondition( smp.done );
	}
	Com_Memset( &smp, 0, sizeof( smp ) );
	glConfig.smpActive = qfalse;
}
