/*
===========================================================================
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

// vk_submit.c -- the submit thread (r_vkSubmitThread 1)
//
// The frames are handed to a thread that submits and presents them, and
// acquires the swapchain image of the next frame right after each present
// (as vk_end_frame does without it), so the main thread doesn't wait in
// vkQueueSubmit, vkAcquireNextImageKHR or vkQueuePresentKHR (on some
// drivers an acquire stalls for a millisecond now and then). A frame is
// drawn into that image when the thread has it by the frame's first render
// pass (VK_TakeSubmitThreadImage), which is the rule; otherwise it is drawn
// into the offscreen color image, and the thread acquires an image (or
// takes the one it acquired since), copies the frame into it in the same
// vkQueueSubmit and presents it. The copy costs GPU time (0.03 ms at
// 640x480, 0.5 ms at 3840x2160 on a Radeon 780M), which is why it is only
// the fallback.
//
// With the render thread (r_smp) the frames it ends are submitted there and
// the thread only presents them (VK_QueuePresentJob): a frame's submission
// then never waits behind the present of the frame before (about 50 us in
// NVIDIA's present), so the GPU starts each frame as soon as it is recorded.
// The queue is shared then: vkQueueSubmit on the render thread and
// vkQueuePresentKHR (and the thread's own submissions) here take queueLock.
// A frame drawn offscreen is copied into the image the thread acquired
// after its last present, in the render thread's submission, once the
// thread is done with that present (the render thread drains it first).
//
// The queue and the swapchain are externally synchronized. While the thread
// has frames to submit, the main thread uses neither: it drains the thread
// first (VK_DrainSubmitThread) to submit on its own (mid-frame flushes,
// vk_finish), to replace or destroy the swapchain, and at shutdown. The
// present fences and the retired swapchains (vk_instance.c) belong to the
// thread while it runs. A frame slot is used again only once the thread is
// done with its frame (VK_JoinSubmitJob) and the frame's fence has
// signaled, so there are no more frames in flight than without it.
//
// The thread calls no ri.* function (the engine's aren't thread-safe):
// what it measured and the result of each call stay with the frame's job
// until the main thread joins the frame, and validation messages raised on
// the thread are printed by the main thread then.

#include "vk_local.h"

#include <SDL3/SDL.h>

cvar_t		*r_vkSubmitThread;

// a frame handed to the thread
typedef struct {
	uint64_t		serial;			// vk.frameSerial of the frame
	VkCommandBuffer	cmds[2];
	uint32_t		numCmds;
	qboolean		present;		// present it; otherwise submit only
	qboolean		presentOnly;	// submitted by the render thread already: present image only
	int				image;			// the swapchain image it was drawn into, -1: the offscreen image, to copy
	VkSemaphore		acquired;		// that image's acquire semaphore, when no submission waited for it yet
	uint64_t		aoSignalValue, aoWaitValue;	// the compute queue's ambient occlusion (VK_TemporalTakeAoSubmitInfo), 0 none
	uint64_t		inputUs;		// r_vkLatency: the frame's input time
	uint64_t		handoffUs;		// when the main thread queued it
	uint64_t		frameStartUs;	// vk.frameStartUs of the frame

	// set by the thread
	VkResult		result;
	qboolean		outOfDate;		// acquire or present: VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR
	qboolean		presented;
	qboolean		copied;			// drawn offscreen and copied
	uint64_t		startUs;		// when the thread started it
	uint64_t		presentAt;		// when it called vkQueuePresentKHR, 0: no present
	uint32_t		submits;
	uint64_t		submitUs, acquireUs, presentUs, presentWaitUs;
} vkSubmitJob_t;

#define VK_THREAD_MESSAGES		16384	// validation messages raised on the thread, until printed

static struct {
	SDL_Thread		*thread;
	SDL_ThreadID	threadId;
	SDL_Mutex		*lock;
	SDL_Mutex		*queueLock;		// vk.queue: the thread's submissions and presents, the render thread's submissions
	SDL_Condition	*wake;			// the thread waits for a job or quit
	SDL_Condition	*done;			// the main thread waits for a job to be done
	qboolean		quit;
	qboolean		stopping;		// vk_shutdown: errors of the last jobs are not raised again

	vkSubmitJob_t	jobs[VK_MAX_FRAMES];	// by frame slot
	qboolean		pending[VK_MAX_FRAMES];	// queued, not joined yet (main thread)
	qboolean		finished[VK_MAX_FRAMES];	// done by the thread
	int				order[VK_MAX_FRAMES];	// the slots of the jobs, in the order they were queued
	uint64_t		queued;			// jobs queued
	uint64_t		taken;			// jobs the thread took (the next one is order[taken % VK_MAX_FRAMES])

	// the image the thread acquired after its last present, for the next
	// frame (lock): the main thread takes it to draw into, or the thread
	// copies a frame drawn offscreen into it
	qboolean		ahead;
	uint32_t		aheadIndex;
	int				aheadSemaphore;	// in vk.acquireSemaphores
	qboolean		alwaysCopy;		// r_vkSubmitThread 2: every frame drawn offscreen and copied, for tests

	SDL_Mutex		*messageLock;
	char			messages[VK_THREAD_MESSAGES];
	int				messagesLength;
	int				messagesLost;

	// a library loaded into the game that measures input to present
	// (tools/rtimer/inlat.c): told which frame each present shows
	void			( *inlatQueued )( unsigned long long serial );
	void			( *inlatPresenting )( unsigned long long serial );
} vk_submit;

/*
====================================================================

THE THREAD

====================================================================
*/

static VkResult VK_ThreadSubmit( vkSubmitJob_t *job, uint32_t count, const VkSubmitInfo *submits, VkFence fence ) {
	uint64_t start = VK_Microseconds();
	VkResult result;

	SDL_LockMutex( vk_submit.queueLock );
	result = qvkQueueSubmit( vk.queue, count, submits, fence );
	SDL_UnlockMutex( vk_submit.queueLock );
	job->submitUs += VK_Microseconds() - start;
	job->submits++;
	return result;
}

/*
===============
VK_ThreadAcquire

Acquires a swapchain image with the next semaphore of the ring of
VK_ACQUIRE_SEMAPHORES, VK_MAX_FRAMES + 1 (the thread acquires and the main
thread doesn't). The acquire for frame k (at its job, or after the present
of frame k - 1) reuses the semaphore of an acquire for frame
k - VK_MAX_FRAMES - 1 or earlier, and the main thread has waited for that
frame's fence before it queued frame k - 1: the submission that waited for
the semaphore has completed.
===============
*/
static VkResult VK_ThreadAcquire( vkSubmitJob_t *job, uint32_t *index, int *semaphore ) {
	int s = vk.nextAcquireSemaphore;
	uint64_t start = VK_Microseconds();
	VkResult result = qvkAcquireNextImageKHR( vk.device, vk.swapchain, UINT64_MAX, vk.acquireSemaphores[s], VK_NULL_HANDLE, index );

	job->acquireUs += VK_Microseconds() - start;
	if ( result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ) {
		*semaphore = s;
		vk.nextAcquireSemaphore = ( s + 1 ) % VK_ACQUIRE_SEMAPHORES;
	}
	if ( result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR ) {
		// replaced by the main thread (VK_SelectTarget); suboptimal is
		// still presentable
		job->outOfDate = qtrue;
		if ( result == VK_SUBOPTIMAL_KHR ) {
			result = VK_SUCCESS;
		}
	}
	return result;
}

/*
===============
VK_ThreadPresent

As VK_QueuePresent (vk.c) on the main thread, then the next present's fence
and the retired swapchains whose presents are done, as vk_end_frame does
without the thread
===============
*/
static VkResult VK_ThreadPresent( vkSubmitJob_t *job, uint32_t imageIndex ) {
	VkSwapchainPresentFenceInfoEXT fenceInfo;
	VkPresentInfoKHR presentInfo;
	VkPresentIdKHR presentId;
	uint64_t id = 0;
	VkResult result;
	uint64_t start;

	Com_Memset( &presentInfo, 0, sizeof( presentInfo ) );
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &vk.presentSemaphores[imageIndex];
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &vk.swapchain;
	presentInfo.pImageIndices = &imageIndex;
	if ( vk.presentFences ) {
		result = VK_TryPreparePresentFence();
		if ( result != VK_SUCCESS ) {
			return result;
		}
		Com_Memset( &fenceInfo, 0, sizeof( fenceInfo ) );
		fenceInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
		fenceInfo.swapchainCount = 1;
		fenceInfo.pFences = &vk.presentFenceSlots[vk.nextPresentFence].fence;
		presentInfo.pNext = &fenceInfo;
	}
	if ( vk.presentWait ) {
		VK_ChainPresentId( &presentInfo, &presentId, &id );
	}
	if ( vk_submit.inlatPresenting ) {
		vk_submit.inlatPresenting( job->serial );
	}

	start = VK_Microseconds();
	job->presentAt = start;
	SDL_LockMutex( vk_submit.queueLock );
	result = qvkQueuePresentKHR( vk.queue, &presentInfo );
	SDL_UnlockMutex( vk_submit.queueLock );
	job->presentUs = VK_Microseconds() - start;
	VK_ProbePresent( job->inputUs, start, id );
	if ( result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ) {
		job->outOfDate = qtrue;
		job->presented = result == VK_SUBOPTIMAL_KHR;
		result = VK_SUCCESS;
	} else {
		job->presented = result == VK_SUCCESS;
	}
	if ( result != VK_SUCCESS ) {
		return result;
	}

	// queued, also when out of date: the semaphore wait happens and the
	// fence signals
	VK_PresentFenceUsed();
	if ( vk.presentFences ) {
		result = VK_TryPreparePresentFence();
		if ( vk.numRetiredSwapchains ) {
			VK_DestroyRetiredSwapchains( qfalse );
		}
	}
	// vsync: until it is shown, before the next image is acquired and the
	// next frame reads its input (the main or render thread waits for this
	// job with FIFO)
	if ( result == VK_SUCCESS && !job->outOfDate ) {
		result = VK_WaitForPresentShown( id, &job->presentWaitUs );
		if ( result == VK_ERROR_OUT_OF_DATE_KHR ) {
			job->outOfDate = qtrue;
			result = VK_SUCCESS;
		}
	}
	return result;
}

/*
===============
VK_ThreadCopyAndPresent

A frame drawn offscreen: the image acquired after the last present (or one
acquired now), the copy into it after the frame's commands in the same
vkQueueSubmit (only the copy waits for the image), and the present. When
the swapchain is out of date the frame is submitted and not presented.
===============
*/
static VkResult VK_ThreadCopyAndPresent( vkSubmitJob_t *job, vkFrame_t *frame ) {
	VkPipelineStageFlags waitStage = VK_PresentCopyStage();
	VkSubmitInfo submits[2];
	vkAoSubmitChain_t aoChain;
	VkSemaphore acquired;
	uint32_t imageIndex;
	int semaphore;
	qboolean ahead;
	VkResult result;

	Com_Memset( submits, 0, sizeof( submits ) );
	submits[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submits[0].commandBufferCount = job->numCmds;
	submits[0].pCommandBuffers = job->cmds;
	VK_TemporalAoChainSubmit( &aoChain, &submits[0], job->aoSignalValue, job->aoWaitValue );

	SDL_LockMutex( vk_submit.lock );
	ahead = vk_submit.ahead;
	imageIndex = vk_submit.aheadIndex;
	semaphore = vk_submit.aheadSemaphore;
	vk_submit.ahead = qfalse;
	SDL_UnlockMutex( vk_submit.lock );
	if ( !ahead ) {
		result = VK_ThreadAcquire( job, &imageIndex, &semaphore );
		if ( result == VK_ERROR_OUT_OF_DATE_KHR ) {
			return VK_ThreadSubmit( job, 1, submits, frame->fence );
		}
		if ( result != VK_SUCCESS ) {
			return result;
		}
	}

	result = VK_RecordPresentCopy( frame, frame->presentCmd, vk.swapchainImages[imageIndex] );
	if ( result != VK_SUCCESS ) {
		return result;
	}
	acquired = vk.acquireSemaphores[semaphore];
	submits[1].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submits[1].waitSemaphoreCount = 1;
	submits[1].pWaitSemaphores = &acquired;
	submits[1].pWaitDstStageMask = &waitStage;
	submits[1].commandBufferCount = 1;
	submits[1].pCommandBuffers = &frame->presentCmd;
	submits[1].signalSemaphoreCount = 1;
	submits[1].pSignalSemaphores = &vk.presentSemaphores[imageIndex];
	result = VK_ThreadSubmit( job, 2, submits, frame->fence );
	if ( result != VK_SUCCESS ) {
		return result;
	}
	job->copied = qtrue;
	return VK_ThreadPresent( job, imageIndex );
}

static void VK_RunSubmitJob( vkSubmitJob_t *job, int slot ) {
	vkFrame_t *frame = &vk.frames[slot];
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit;
	vkAoSubmitChain_t aoChain;
	uint32_t imageIndex;
	int semaphore;

	job->startUs = VK_Microseconds();

	Com_Memset( &submit, 0, sizeof( submit ) );
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = job->numCmds;
	submit.pCommandBuffers = job->cmds;
	if ( job->presentOnly ) {
		// submitted by the render thread, which signals the image's present
		// semaphore
		job->result = VK_ThreadPresent( job, job->image );
	} else if ( job->image >= 0 ) {
		// drawn into the swapchain image, which went to the presentation
		// layout at its end; the first submission that used the image waits
		// for its semaphore (VK_SelectTarget's barrier waits at the same stage)
		if ( job->acquired ) {
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &job->acquired;
			submit.pWaitDstStageMask = &waitStage;
		}
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &vk.presentSemaphores[job->image];
		VK_TemporalAoChainSubmit( &aoChain, &submit, job->aoSignalValue, job->aoWaitValue );
		job->result = VK_ThreadSubmit( job, 1, &submit, frame->fence );
		if ( job->result == VK_SUCCESS ) {
			job->result = VK_ThreadPresent( job, job->image );
		}
	} else if ( job->present ) {
		job->result = VK_ThreadCopyAndPresent( job, frame );
	} else {
		// minimized, or the frame drew nothing: the window keeps the last frame
		VK_TemporalAoChainSubmit( &aoChain, &submit, job->aoSignalValue, job->aoWaitValue );
		job->result = VK_ThreadSubmit( job, 1, &submit, frame->fence );
	}

	// the image of the next frame, right after the present: with vsync this
	// is where the thread waits for the display, and the main thread waits
	// for this job at the end of the frame (vk_end_frame), before the next
	// frame's input, as it waits in this acquire without the thread
	if ( job->result == VK_SUCCESS && job->presentAt && !job->outOfDate && vk.swapchain ) {
		qboolean ahead;

		SDL_LockMutex( vk_submit.lock );
		ahead = vk_submit.ahead;
		SDL_UnlockMutex( vk_submit.lock );
		if ( !ahead ) {
			VkResult result = VK_ThreadAcquire( job, &imageIndex, &semaphore );

			if ( result == VK_SUCCESS ) {
				SDL_LockMutex( vk_submit.lock );
				vk_submit.ahead = qtrue;
				vk_submit.aheadIndex = imageIndex;
				vk_submit.aheadSemaphore = semaphore;
				SDL_UnlockMutex( vk_submit.lock );
			} else if ( result != VK_ERROR_OUT_OF_DATE_KHR ) {
				job->result = result;
			}
		}
	}
}

static int SDLCALL VK_SubmitThreadMain( void *data ) {
	(void)data;

	SDL_LockMutex( vk_submit.lock );
	for ( ;; ) {
		int slot;

		while ( vk_submit.taken == vk_submit.queued && !vk_submit.quit ) {
			SDL_WaitCondition( vk_submit.wake, vk_submit.lock );
		}
		if ( vk_submit.taken == vk_submit.queued ) {
			break;	// quit, every job done
		}
		slot = vk_submit.order[vk_submit.taken % VK_MAX_FRAMES];
		SDL_UnlockMutex( vk_submit.lock );

		VK_RunSubmitJob( &vk_submit.jobs[slot], slot );

		SDL_LockMutex( vk_submit.lock );
		vk_submit.taken++;
		vk_submit.finished[slot] = qtrue;
		// every waiter: with r_smp the render thread and the main thread can
		// both wait for a frame (one woken alone left the other asleep)
		SDL_BroadcastCondition( vk_submit.done );
	}
	SDL_UnlockMutex( vk_submit.lock );
	return 0;
}

/*
====================================================================

THE MAIN THREAD

====================================================================
*/

// validation messages raised on the thread (VK_DebugCallback), printed
// when the main thread joins a frame
qboolean VK_DeferThreadMessage( const char *text ) {
	int length;

	if ( !vk_submit.thread || SDL_GetCurrentThreadID() != vk_submit.threadId ) {
		return qfalse;
	}
	length = (int)strlen( text );
	SDL_LockMutex( vk_submit.messageLock );
	if ( vk_submit.messagesLength + length + 1 < VK_THREAD_MESSAGES ) {
		Com_Memcpy( vk_submit.messages + vk_submit.messagesLength, text, length + 1 );
		vk_submit.messagesLength += length + 1;
	} else {
		vk_submit.messagesLost++;
	}
	SDL_UnlockMutex( vk_submit.messageLock );
	return qtrue;
}

static void VK_PrintThreadMessages( void ) {
	char *text;
	int i, length, lost;

	if ( !vk_submit.messagesLength && !vk_submit.messagesLost ) {
		return;
	}
	SDL_LockMutex( vk_submit.messageLock );
	length = vk_submit.messagesLength;
	lost = vk_submit.messagesLost;
	text = length ? ri.Malloc( length ) : NULL;
	if ( text ) {
		Com_Memcpy( text, vk_submit.messages, length );
	}
	vk_submit.messagesLength = 0;
	vk_submit.messagesLost = 0;
	SDL_UnlockMutex( vk_submit.messageLock );

	for ( i = 0; text && i < length; i += (int)strlen( text + i ) + 1 ) {
		ri.Printf( PRINT_ALL, "%s (submit thread)\n", text + i );
	}
	if ( lost ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "Vulkan validation: %d more messages on the submit thread\n", lost );
	}
	if ( text ) {
		ri.Free( text );
	}
}

/*
===============
VK_TakeSubmitJob

A job the thread has finished: its counters, its frame log record, and the
errors of its calls
===============
*/
static void VK_TakeSubmitJob( int slot ) {
	const vkSubmitJob_t *job = &vk_submit.jobs[slot];

	vk_submit.pending[slot] = qfalse;
	vk.stats.threadFrames++;
	vk.stats.threadStartUs += job->startUs - job->handoffUs;
	vk.stats.threadSubmits += job->submits;
	vk.stats.threadSubmitUs += job->submitUs;
	vk.stats.threadAcquireUs += job->acquireUs;
	vk.stats.threadPresentUs += job->presentUs;
	vk.stats.threadPresentWaitUs += job->presentWaitUs;
	if ( job->presentAt ) {
		vk.stats.threadPresents++;
		vk.stats.frameToPresentUs += job->presentAt - job->frameStartUs;
	}
	if ( job->outOfDate ) {
		// replaced at the next frame's first render pass (VK_SelectTarget)
		vk.swapchainOutOfDate = qtrue;
	}
	if ( job->copied ) {
		vk.stats.threadCopies++;
	}
	if ( vk_frameLog ) {
		VK_FrameLogThread( job->serial, job->presentAt, job->startUs - job->handoffUs, job->submitUs, job->acquireUs, job->presentUs );
	}

	VK_PrintThreadMessages();
	if ( !vk_submit.stopping ) {
		VK_Check( job->result, "the submit thread" );
	}
}

/*
===============
VK_JoinSubmitJob

Before a frame slot is used again: waits until the thread is done with its
frame
===============
*/
void VK_JoinSubmitJob( int slot ) {
	uint64_t start;

	if ( !vk_submit.thread || !vk_submit.pending[slot] ) {
		return;
	}
	start = VK_Microseconds();
	SDL_LockMutex( vk_submit.lock );
	while ( !vk_submit.finished[slot] ) {
		SDL_WaitCondition( vk_submit.done, vk_submit.lock );
	}
	SDL_UnlockMutex( vk_submit.lock );
	vk.stats.threadWaitUs += VK_Microseconds() - start;
	VK_TakeSubmitJob( slot );
}

// the jobs the thread has finished, without waiting
void VK_CollectSubmitJobs( void ) {
	qboolean finished[VK_MAX_FRAMES];
	int i;

	if ( !vk_submit.thread ) {
		return;
	}
	SDL_LockMutex( vk_submit.lock );
	Com_Memcpy( finished, vk_submit.finished, sizeof( finished ) );
	SDL_UnlockMutex( vk_submit.lock );
	for ( i = 0; i < VK_MAX_FRAMES; i++ ) {
		if ( vk_submit.pending[i] && finished[i] ) {
			VK_TakeSubmitJob( i );
		}
	}
}

/*
===============
VK_DrainSubmitThread

Waits until the thread has done every frame queued: the queue and the
swapchain can then be used on this thread until the next frame is queued
===============
*/
void VK_DrainSubmitThread( void ) {
	uint64_t i;

	if ( !vk_submit.thread ) {
		return;
	}
	// the last VK_MAX_FRAMES jobs: the slots of the older ones were joined
	// before they were used again
	for ( i = vk_submit.queued > VK_MAX_FRAMES ? vk_submit.queued - VK_MAX_FRAMES : 0; i < vk_submit.queued; i++ ) {
		VK_JoinSubmitJob( vk_submit.order[i % VK_MAX_FRAMES] );
	}
}

/*
===============
VK_TakeSubmitThreadImage

At the first render pass of a frame: the swapchain image the thread
acquired after its last present, to draw the frame into, if it has one
(without waiting); the frame's first submission waits for its semaphore
===============
*/
qboolean VK_TakeSubmitThreadImage( uint32_t *index, int *semaphore ) {
	qboolean ahead;

	if ( !vk_submit.thread || vk_submit.alwaysCopy ) {
		return qfalse;
	}
	SDL_LockMutex( vk_submit.lock );
	ahead = vk_submit.ahead;
	if ( ahead ) {
		*index = vk_submit.aheadIndex;
		*semaphore = vk_submit.aheadSemaphore;
		vk_submit.ahead = qfalse;
	}
	SDL_UnlockMutex( vk_submit.lock );
	return ahead;
}

/*
===============
VK_ReclaimSubmitThreadImage

The thread drained: the image it acquired ahead becomes the main thread's
held image (vk.heldImage), which VK_ReleaseHeldImage hands back before the
swapchain is replaced or destroyed; the main thread holds none then
===============
*/
void VK_ReclaimSubmitThreadImage( void ) {
	if ( !vk_submit.ahead ) {
		return;
	}
	if ( vk.heldImage ) {
		VK_Check( VK_ERROR_UNKNOWN, "two images held" );
	}
	vk.heldImage = qtrue;
	vk.heldIndex = vk_submit.aheadIndex;
	vk.heldSemaphore = vk_submit.aheadSemaphore;
	vk_submit.ahead = qfalse;
}

/*
===============
VK_QueueSubmitJob

Hands the frame to the thread: its command buffers (ended), and whether to
present it. The frame's fence has been reset.
===============
*/
void VK_QueueSubmitJob( vkFrame_t *frame, const VkCommandBuffer *cmds, uint32_t numCmds, qboolean present, int image, VkSemaphore acquired ) {
	int slot = (int)( frame - vk.frames );
	vkSubmitJob_t *job = &vk_submit.jobs[slot];

	Com_Memset( job, 0, sizeof( *job ) );
	job->serial = frame->serial;
	Com_Memcpy( job->cmds, cmds, numCmds * sizeof( cmds[0] ) );
	job->numCmds = numCmds;
	job->present = present;
	job->image = image;
	job->acquired = acquired;
	job->frameStartUs = vk.frameStartUs;
	job->inputUs = frame->probeInputUs;
	// the compute queue's ambient occlusion (r_aoAsync): taken here, on
	// the main thread
	VK_TemporalTakeAoSubmitInfo( &job->aoSignalValue, &job->aoWaitValue );
	if ( present && vk_submit.inlatQueued ) {
		vk_submit.inlatQueued( job->serial );
	}
	vk_submit.pending[slot] = qtrue;

	SDL_LockMutex( vk_submit.lock );
	job->handoffUs = VK_Microseconds();
	vk_submit.finished[slot] = qfalse;
	vk_submit.order[vk_submit.queued % VK_MAX_FRAMES] = slot;
	vk_submit.queued++;
	SDL_SignalCondition( vk_submit.wake );
	SDL_UnlockMutex( vk_submit.lock );
}

/*
===============
VK_QueuePresentJob

The render thread submitted the frame itself (its submission signals the
present semaphore of image): the thread presents it, then acquires the
image of the next frame as after any present
===============
*/
void VK_QueuePresentJob( vkFrame_t *frame, int image ) {
	int slot = (int)( frame - vk.frames );
	vkSubmitJob_t *job = &vk_submit.jobs[slot];

	Com_Memset( job, 0, sizeof( *job ) );
	job->serial = frame->serial;
	job->present = qtrue;
	job->presentOnly = qtrue;
	job->image = image;
	job->frameStartUs = vk.frameStartUs;
	job->inputUs = frame->probeInputUs;
	if ( vk_submit.inlatQueued ) {
		vk_submit.inlatQueued( job->serial );
	}
	vk_submit.pending[slot] = qtrue;

	SDL_LockMutex( vk_submit.lock );
	job->handoffUs = VK_Microseconds();
	vk_submit.finished[slot] = qfalse;
	vk_submit.order[vk_submit.queued % VK_MAX_FRAMES] = slot;
	vk_submit.queued++;
	SDL_SignalCondition( vk_submit.wake );
	SDL_UnlockMutex( vk_submit.lock );
}

/*
===============
VK_JoinSubmittingJobs

Before the render thread submits a frame itself: the frames handed over
whole (ended on the main thread, as while loading) are submitted first, in
their order
===============
*/
void VK_JoinSubmittingJobs( void ) {
	uint64_t i;

	if ( !vk_submit.thread ) {
		return;
	}
	for ( i = vk_submit.queued > VK_MAX_FRAMES ? vk_submit.queued - VK_MAX_FRAMES : 0; i < vk_submit.queued; i++ ) {
		int slot = vk_submit.order[i % VK_MAX_FRAMES];

		if ( vk_submit.pending[slot] && !vk_submit.jobs[slot].presentOnly ) {
			VK_JoinSubmitJob( slot );
		}
	}
}

// vk.queue while the thread may be presenting (the render thread's
// submissions, VK_QueueSubmit in vk.c)
void VK_LockSubmitQueue( void ) {
	if ( vk_submit.queueLock ) {
		SDL_LockMutex( vk_submit.queueLock );
	}
}

void VK_UnlockSubmitQueue( void ) {
	if ( vk_submit.queueLock ) {
		SDL_UnlockMutex( vk_submit.queueLock );
	}
}

/*
===============
VK_StartSubmitThread

At vk_initialize, before the frames and the swapchain are created
===============
*/
qboolean VK_StartSubmitThread( void ) {
	Com_Memset( &vk_submit, 0, sizeof( vk_submit ) );
	vk_submit.lock = SDL_CreateMutex();
	vk_submit.queueLock = SDL_CreateMutex();
	vk_submit.messageLock = SDL_CreateMutex();
	vk_submit.wake = SDL_CreateCondition();
	vk_submit.done = SDL_CreateCondition();
	if ( vk_submit.lock && vk_submit.queueLock && vk_submit.messageLock && vk_submit.wake && vk_submit.done ) {
		vk_submit.thread = SDL_CreateThread( VK_SubmitThreadMain, "vksubmit", NULL );
	}
	if ( !vk_submit.thread ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: Vulkan: no submit thread (%s)\n", SDL_GetError() );
		VK_StopSubmitThread();
		return qfalse;
	}
	vk_submit.threadId = SDL_GetThreadID( vk_submit.thread );
	vk_submit.alwaysCopy = r_vkSubmitThread->integer == 2;

#ifndef _WIN32
	// tools/rtimer/inlat.c, when it is preloaded
	{
		SDL_SharedObject *game = SDL_LoadObject( NULL );

		if ( game ) {
			vk_submit.inlatQueued = (void ( * )( unsigned long long ))SDL_LoadFunction( game, "inlat_frame_queued" );
			vk_submit.inlatPresenting = (void ( * )( unsigned long long ))SDL_LoadFunction( game, "inlat_frame_presenting" );
			if ( !vk_submit.inlatQueued || !vk_submit.inlatPresenting ) {
				vk_submit.inlatQueued = NULL;
				vk_submit.inlatPresenting = NULL;
			}
			SDL_UnloadObject( game );
			SDL_ClearError();
		}
	}
#endif
	return qtrue;
}

/*
===============
VK_StopSubmitThread

At vk_shutdown: every frame queued is submitted, then the thread ends
===============
*/
void VK_StopSubmitThread( void ) {
	if ( vk_submit.thread ) {
		// also after a fatal error: what is queued may wait for nothing else
		vk_submit.stopping = qtrue;
		VK_DrainSubmitThread();
		SDL_LockMutex( vk_submit.lock );
		vk_submit.quit = qtrue;
		SDL_SignalCondition( vk_submit.wake );
		SDL_UnlockMutex( vk_submit.lock );
		SDL_WaitThread( vk_submit.thread, NULL );
		vk_submit.thread = NULL;
		VK_PrintThreadMessages();
		// released with the swapchain (VK_DestroySwapchain)
		VK_ReclaimSubmitThreadImage();
	}
	if ( vk_submit.wake ) {
		SDL_DestroyCondition( vk_submit.wake );
	}
	if ( vk_submit.done ) {
		SDL_DestroyCondition( vk_submit.done );
	}
	if ( vk_submit.lock ) {
		SDL_DestroyMutex( vk_submit.lock );
	}
	if ( vk_submit.queueLock ) {
		SDL_DestroyMutex( vk_submit.queueLock );
	}
	if ( vk_submit.messageLock ) {
		SDL_DestroyMutex( vk_submit.messageLock );
	}
	Com_Memset( &vk_submit, 0, sizeof( vk_submit ) );
}
