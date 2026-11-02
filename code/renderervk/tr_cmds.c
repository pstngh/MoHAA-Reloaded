/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#include "tr_local.h"

static backEndCounters_t pc_save;

/*
=====================
R_SavePerformanceCounters
=====================
*/
void R_SavePerformanceCounters(void) {
    memcpy(&pc_save, &backEnd.pc, sizeof(pc_save));
}

/*
=====================
R_PerformanceCounters
=====================
*/
void R_PerformanceCounters( void ) {
	if (r_fps->integer) {
		ri.SetPerformanceCounters(
			backEnd.pc.c_totalIndexes / 3,
			backEnd.pc.c_vertexes,
			R_SumOfUsedImages(),
			pc_save.c_totalIndexes / 3,
			pc_save.c_vertexes,
			backEnd.pc.c_characterlights
			);
	}
	if ( !r_speeds->integer ) {
		// clear the counters even if we aren't printing
		Com_Memset( &tr.pc, 0, sizeof( tr.pc ) );
		Com_Memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
		return;
	}

	if (r_speeds->integer == 1) {
		ri.Printf (PRINT_ALL, "%i/%i shaders/surfs %i leafs %i verts %i/%i tris %.2f mtex %.2f dc\n",
			backEnd.pc.c_shaders, backEnd.pc.c_surfaces, tr.pc.c_leafs, backEnd.pc.c_vertexes, 
			backEnd.pc.c_indexes/3, backEnd.pc.c_totalIndexes/3, 
			R_SumOfUsedImages()/(1000000.0f), backEnd.pc.c_overDraw / (float)(glConfig.vidWidth * glConfig.vidHeight) ); 
	} else if (r_speeds->integer == 2) {
		ri.Printf (PRINT_ALL, "(patch) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
			tr.pc.c_sphere_cull_patch_in, tr.pc.c_sphere_cull_patch_clip, tr.pc.c_sphere_cull_patch_out, 
			tr.pc.c_box_cull_patch_in, tr.pc.c_box_cull_patch_clip, tr.pc.c_box_cull_patch_out );
		ri.Printf (PRINT_ALL, "(md3) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
			tr.pc.c_sphere_cull_md3_in, tr.pc.c_sphere_cull_md3_clip, tr.pc.c_sphere_cull_md3_out, 
			tr.pc.c_box_cull_md3_in, tr.pc.c_box_cull_md3_clip, tr.pc.c_box_cull_md3_out );
	} else if (r_speeds->integer == 3) {
		ri.Printf (PRINT_ALL, "viewcluster: %i\n", tr.viewCluster );
	} else if (r_speeds->integer == 4) {
		if ( backEnd.pc.c_dlightVertexes ) {
			ri.Printf (PRINT_ALL, "dlight srf:%i  culled:%i  verts:%i  tris:%i\n", 
				tr.pc.c_dlightSurfaces, tr.pc.c_dlightSurfacesCulled,
				backEnd.pc.c_dlightVertexes, backEnd.pc.c_dlightIndexes / 3 );
		}
	} 
	else if (r_speeds->integer == 5 )
	{
		ri.Printf( PRINT_ALL, "zFar: %.0f\n", tr.viewParms.zFar );
	}
	else if (r_speeds->integer == 6 )
	{
		ri.Printf( PRINT_ALL, "flare adds:%i tests:%i renders:%i\n", 
			backEnd.pc.c_flareAdds, backEnd.pc.c_flareTests, backEnd.pc.c_flareRenders );
	}

	Com_Memset( &tr.pc, 0, sizeof( tr.pc ) );
	Com_Memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
}


/*
====================
R_IssueRenderCommands
====================
*/
void R_IssueRenderCommands( qboolean runPerformanceCounters ) {
	renderCommandList_t	*cmdList;

	// the 2D draws made before come first (r_smp: the thread ends them, at
	// RC_END_2D_BATCH or the swap)
	if ( !glConfig.smpActive ) {
		R_End2DBatch();
	}

	cmdList = &backEndData->commands;
	assert(cmdList);
	// add an end-of-list command
	*(int *)(cmdList->cmds + cmdList->used) = RC_END_OF_LIST;

	// clear it out, in case this is a sync and not a buffer flip
	cmdList->used = 0;

	if ( runPerformanceCounters ) {
		R_PerformanceCounters();
	}

	// actually start the commands going
	if ( !r_skipBackEnd->integer ) {
		// let it start on the new batch
		if ( glConfig.smpActive ) {
			// a list with a screenshot, a video frame or the overdraw
			// readback uses the engine's temporary memory and files on the
			// thread: the front end waits for it (R_QueueScreenshot)
			qboolean wait = backEndData->syncAfterIssue;

			backEndData->syncAfterIssue = qfalse;
			if ( runPerformanceCounters ) {
				// the frame's list (RE_EndFrame), and the waits before the
				// next frame's input
				R_IssueFrameThreaded( cmdList->cmds );
			} else {
				R_IssueRenderCommandsThreaded( cmdList->cmds );
			}
			if ( wait ) {
				R_SyncRenderThread();
			}
		} else {
			RB_ExecuteRenderCommands( cmdList->cmds );
		}
	}
}


/*
====================
R_IssuePendingRenderCommands

Issue any pending commands and wait for them to complete. Nothing to do
when none is queued: the 2D exports call it before each of their draws.
Everything that uses the back end outside the 2D draws calls it, so the
open 2D batch is drawn first (see tr_draw.c)
====================
*/
void R_IssuePendingRenderCommands( void ) {
	// a command's body on the render thread: the list runs in order, and the
	// open 2D batch is drawn here as it was (R_End2DBatch below)
	if ( R_OnRenderThread() ) {
		R_End2DBatch();
		return;
	}
	if ( glConfig.smpActive ) {
		// the thread runs what is queued, the open 2D batch last, and the
		// front end owns the back end's state until the next hand-over
		if ( tr.registered && backEndData->commands.used ) {
			end2DBatchCommand_t *cmd = R_GetCommandBufferReserved( sizeof( *cmd ), 0 );

			if ( cmd ) {
				cmd->commandId = RC_END_2D_BATCH;
			}
			R_IssueRenderCommands( qfalse );
		}
		R_SyncRenderThread();
		R_End2DBatch();
		return;
	}
	R_End2DBatch();
	if ( !tr.registered || !backEndData->commands.used ) {
		return;
	}
	R_IssueRenderCommands( qfalse );
}

/*
============
R_GetCommandBufferReserved

make sure there is enough command space
============
*/
void *R_GetCommandBufferReserved( int bytes, int reservedBytes ) {
	renderCommandList_t	*cmdList;

	cmdList = &backEndData->commands;
	bytes = PAD(bytes, sizeof(void *));

	// always leave room for the end of list command
	if ( cmdList->used + bytes + sizeof( int ) + reservedBytes > MAX_RENDER_COMMANDS ) {
		if ( bytes > MAX_RENDER_COMMANDS - sizeof( int ) ) {
			ri.Error( ERR_FATAL, "R_GetCommandBuffer: bad size %i", bytes );
		}
		// if we run out of room, just start dropping commands
		ri.Printf( PRINT_WARNING, "Failed to allocate render command of size %d\n", bytes );
		return NULL;
	}

	cmdList->used += bytes;

	return cmdList->cmds + cmdList->used - bytes;
}

/*
=============
R_GetCommandBuffer

returns NULL if there is not enough space for important commands
=============
*/
void *R_GetCommandBuffer( int bytes ) {
	return R_GetCommandBufferReserved( bytes, PAD( sizeof( swapBuffersCommand_t ), sizeof(void *) ) );
}


/*
=============
R_AddDrawSurfCmd

=============
*/
void	R_AddDrawSurfCmd( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	drawSurfsCommand_t	*cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_DRAW_SURFS;

	cmd->drawSurfs = drawSurfs;
	cmd->numDrawSurfs = numDrawSurfs;

	cmd->refdef = tr.refdef;
	cmd->viewParms = tr.viewParms;
	cmd->viewParms.skyRendered = tr.skyRendered;
	cmd->viewParms.portalRendered = tr.portalRendered;
	cmd->viewParms.farclip = tr.farclip;
	// r_smp: the terrain slots this scene changed, for the thread's pool copy
	cmd->terrainChanges = backEndData->terrainChanges + backEndData->firstTerrainChange;
	cmd->numTerrainChanges = backEndData->numTerrainChanges - backEndData->firstTerrainChange;
	cmd->terrainChangesAll = backEndData->terrainChangesAll;
	backEndData->firstTerrainChange = backEndData->numTerrainChanges;
	backEndData->terrainChangesAll = qfalse;
}


/*
=============
R_AddSpriteSurfCmd

=============
*/
void	R_AddSpriteSurfCmd(drawSurf_t* drawSurfs, int numDrawSurfs) {
    drawSurfsCommand_t* cmd;

    cmd = R_GetCommandBuffer(sizeof(*cmd));
    if (!cmd) {
        return;
    }
    cmd->commandId = RC_SPRITE_SURFS;

    cmd->drawSurfs = drawSurfs;
    cmd->numDrawSurfs = numDrawSurfs;

    cmd->refdef = tr.refdef;
    cmd->viewParms = tr.viewParms;
    cmd->terrainChanges = NULL;
    cmd->numTerrainChanges = 0;
    cmd->terrainChangesAll = qfalse;
}


/*
=============
RE_SetColor

Passing NULL will set the color to white
=============
*/
void	RE_SetColor( const float *rgba ) {
	setColorCommand_t	*cmd;

  if ( !tr.registered ) {
    return;
  }
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SET_COLOR;
	if ( !rgba ) {
		static float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

		rgba = white;
	}

	cmd->color[0] = rgba[0];
	cmd->color[1] = rgba[1];
	cmd->color[2] = rgba[2];
	cmd->color[3] = rgba[3];
}


/*
=============
RE_StretchPic
=============
*/
void RE_StretchPic ( float x, float y, float w, float h, 
					  float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	stretchPicCommand_t	*cmd;

  if (!tr.registered) {
    return;
  }
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_STRETCH_PIC;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;
}

#define MODE_RED_CYAN	1
#define MODE_RED_BLUE	2
#define MODE_RED_GREEN	3
#define MODE_GREEN_MAGENTA 4
#define MODE_MAX	MODE_GREEN_MAGENTA

void R_SetColorMode(qboolean *rgba, stereoFrame_t stereoFrame, int colormode)
{
	rgba[0] = rgba[1] = rgba[2] = rgba[3] = qtrue;
	
	if(colormode > MODE_MAX)
	{
		if(stereoFrame == STEREO_LEFT)
			stereoFrame = STEREO_RIGHT;
		else if(stereoFrame == STEREO_RIGHT)
			stereoFrame = STEREO_LEFT;
		
		colormode -= MODE_MAX;
	}
	
	if(colormode == MODE_GREEN_MAGENTA)
	{
		if(stereoFrame == STEREO_LEFT)
			rgba[0] = rgba[2] = qfalse;
		else if(stereoFrame == STEREO_RIGHT)
			rgba[1] = qfalse;
	}
	else
	{
		if(stereoFrame == STEREO_LEFT)
			rgba[1] = rgba[2] = qfalse;
		else if(stereoFrame == STEREO_RIGHT)
		{
			rgba[0] = qfalse;
		
			if(colormode == MODE_RED_BLUE)
				rgba[1] = qfalse;
			else if(colormode == MODE_RED_GREEN)
				rgba[2] = qfalse;
		}
	}
}


/*
====================
RE_BeginFrame

If running in stereo, RE_BeginFrame will be called twice
for each RE_EndFrame
====================
*/
void RE_BeginFrame( stereoFrame_t stereoFrame ) {
	drawBufferCommand_t	*cmd = NULL;
	colorMaskCommand_t *colcmd = NULL;

	if ( !tr.registered ) {
		return;
	}
	// the screenshots, video frames and overdraw sums the GPU has done
	R_ProcessReadbacks();

	// the UI also draws between frames (when a menu is pushed): drawn before
	// the frame begins, as each of those draws was
	if ( glConfig.smpActive ) {
		end2DBatchCommand_t *batchCmd;

		// with vsync the frame before was waited for at its hand-over
		// (R_IssueFrameThreaded), before this frame read its input
		batchCmd = R_GetCommandBuffer( sizeof( *batchCmd ) );
		if ( batchCmd ) {
			batchCmd->commandId = RC_END_2D_BATCH;
		}
	} else {
		R_End2DBatch();
	}

	tr.frameCount++;
	tr.frameSceneNum = 0;

	//
	// do overdraw measurement
	//
	if ( r_measureOverdraw->integer )
	{
		if ( glConfig.stencilBits < 4 )
		{
			ri.Printf( PRINT_ALL, "Warning: not enough stencil bits to measure overdraw: %d\n", glConfig.stencilBits );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		}
		else if ( r_shadows->integer == 2 )
		{
			ri.Printf( PRINT_ALL, "Warning: stencil shadows and overdraw measurement are mutually exclusive\n" );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		}
		else
		{
			// for the stencil readback at the end of the frame
			// (RB_SwapBuffers), before anything starts the frame
			R_FrontEndSync();
			vk_want_depth_reads();
			R_IssuePendingRenderCommands();
			vk_set_stencil_test( qtrue );
			vk_set_stencil_write_mask( ~0U );
			vk_set_stencil_func( STENCILFUNC_ALWAYS, 0U, ~0U );
			vk_set_stencil_op( STENCILOP_KEEP, STENCILOP_INCR, STENCILOP_INCR );
		}
		r_measureOverdraw->modified = qfalse;
	}
	else
	{
		// this is only reached if it was on and is now off
		if ( r_measureOverdraw->modified ) {
			R_IssuePendingRenderCommands();
			vk_set_stencil_test( qfalse );
		}
		r_measureOverdraw->modified = qfalse;
	}

	//
	// texturemode stuff
	//
	if ( r_textureMode->modified ) {
		R_IssuePendingRenderCommands();
		R_TextureMode( r_textureMode->string );
		r_textureMode->modified = qfalse;
	}

	//
	// gamma stuff
	//
	if ( r_gamma->modified ) {
		r_gamma->modified = qfalse;

		R_IssuePendingRenderCommands();
		R_SetColorMappings();
	}

	if (glConfig.stereoEnabled) {
		if( !(cmd = R_GetCommandBuffer(sizeof(*cmd))) )
			return;
			
		cmd->commandId = RC_DRAW_BUFFER;
		
		if ( stereoFrame == STEREO_LEFT ) {
			cmd->buffer = DRAWBUFFER_BACK_LEFT;
		} else if ( stereoFrame == STEREO_RIGHT ) {
			cmd->buffer = DRAWBUFFER_BACK_RIGHT;
		} else {
			ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );
		}
	}
	else
	{
		if(r_anaglyphMode->integer)
		{
			if(r_anaglyphMode->modified)
			{
				// clear the back buffer
				R_FrontEndSync();
				vk_set_color_mask(qtrue, qtrue, qtrue, qtrue);
				vk_clear(CLEAR_COLOR, colorBlack);
				
				r_anaglyphMode->modified = qfalse;
			}
			
			if(stereoFrame == STEREO_LEFT)
			{
				if( !(cmd = R_GetCommandBuffer(sizeof(*cmd))) )
					return;
				
				if( !(colcmd = R_GetCommandBuffer(sizeof(*colcmd))) )
					return;
			}
			else if(stereoFrame == STEREO_RIGHT)
			{
				clearDepthCommand_t *cldcmd;
				
				if( !(cldcmd = R_GetCommandBuffer(sizeof(*cldcmd))) )
					return;

				cldcmd->commandId = RC_CLEARDEPTH;

				if( !(colcmd = R_GetCommandBuffer(sizeof(*colcmd))) )
					return;
			}
			else
				ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );

			R_SetColorMode(colcmd->rgba, stereoFrame, r_anaglyphMode->integer);
			colcmd->commandId = RC_COLORMASK;
		}
		else
		{
			if(stereoFrame != STEREO_CENTER)
				ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is disabled, but stereoFrame was %i", stereoFrame );

			if( !(cmd = R_GetCommandBuffer(sizeof(*cmd))) )
				return;
		}

		if(cmd)
		{
			cmd->commandId = RC_DRAW_BUFFER;

			if(r_anaglyphMode->modified)
			{
				R_FrontEndSync();
				vk_set_color_mask(qtrue, qtrue, qtrue, qtrue);
				r_anaglyphMode->modified = qfalse;
			}

			if (!Q_stricmp(r_drawBuffer->string, "GL_FRONT"))
				cmd->buffer = DRAWBUFFER_FRONT;
			else
				cmd->buffer = DRAWBUFFER_BACK;
		}
	}
	
	tr.refdef.stereoFrame = stereoFrame;
}


/*
=============
RE_EndFrame

Returns the number of msec spent in the back end
=============
*/
void RE_EndFrame( int *frontEndMsec, int *backEndMsec ) {
	swapBuffersCommand_t	*cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = R_GetCommandBufferReserved( sizeof( *cmd ), 0 );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SWAP_BUFFERS;
	cmd->inputUs = vk_probe_input_time();
	cmd->frameToken = ri.FrameToken();

	R_IssueRenderCommands( qtrue );
	vk_probe_frame_end();

	R_InitNextFrame();

	if ( frontEndMsec ) {
		*frontEndMsec = tr.frontEndMsec;
	}
	tr.frontEndMsec = 0;
	if ( backEndMsec ) {
		*backEndMsec = glConfig.smpActive ? R_RenderThreadBackEndMsec() : backEnd.pc.msec;
	}
	if ( !glConfig.smpActive ) {
		backEnd.pc.msec = 0;
	}
}

/*
=============
RE_TakeVideoFrame
=============
*/
void RE_TakeVideoFrame( int width, int height,
		byte *captureBuffer, byte *encodeBuffer, qboolean motionJpeg )
{
	videoFrameCommand_t	*cmd;

	if( !tr.registered ) {
		return;
	}

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if( !cmd ) {
		return;
	}

	cmd->commandId = RC_VIDEOFRAME;

	cmd->width = width;
	cmd->height = height;
	cmd->captureBuffer = captureBuffer;
	cmd->encodeBuffer = encodeBuffer;
	cmd->motionJpeg = motionJpeg;
}
