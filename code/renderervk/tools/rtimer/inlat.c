/*
 * libinlat.so -- the time from input read to present of every frame, and
 * optionally what the presentation engine and the display did with it
 *
 * Preloaded (LD_PRELOAD) into the game, with any renderer and engine
 * build, SDL 3 or SDL 2 (the builds before the SDL 3 port). A frame reads
 * input at its start (Com_Frame: the frame limiter's wait, then IN_Frame,
 * whose SDL_PollEvent loop takes the window system's events: SDL 3 at the
 * first call of the loop, SDL 2 at every call; since Plan 36 the wait's
 * IN_PumpEvents takes what came until its polling started, the loop what
 * came after) and reaches the display at
 * its present: the Vulkan renderer's
 * vkQueuePresentKHR (its entry point comes from
 * SDL_Vulkan_GetVkGetInstanceProcAddr, then vkGetDeviceProcAddr, both
 * wrapped here). For every present it records
 * the time of the first SDL_PollEvent call since the previous present (the
 * frame's input read; 0 when there was none, as in loading screens) and of
 * the present call, with CLOCK_MONOTONIC (the clock of renderer_timed.so's
 * frame files), and writes them at exit to $INLAT_FRAMES:
 *
 *   # inlat frames 1: <n> frames (code/renderervk/tools/rtimer/inlat.c)
 *   # columns, CLOCK_MONOTONIC microseconds: frame read present
 *
 * Every frame since startup is in the file; tools/framelog.py --batch keeps
 * those renderer_timed.so measured and prints the input to present times
 * and the intervals between input reads and between presents. tools/perf.py
 * --framelog sets INLAT_FRAMES. Built by build.sh.
 *
 * The Vulkan renderer's submit thread (r_vkSubmitThread 1) presents on a
 * thread of its own, when the main thread may have read the next frame's
 * input already. The renderer then calls inlat_frame_queued(serial) on the
 * main thread when it hands a frame to be presented to that thread (the
 * frame's read is the first SDL_PollEvent since the last frame handed
 * over) and inlat_frame_presenting(serial) on that thread right before its
 * vkQueuePresentKHR, and the present gets the read of its frame.
 *
 * Present timing (INLAT_DISPLAY=1; nothing of it runs otherwise): what
 * happened to each Vulkan present after the call, from what the window
 * system and the kernel tell an unprivileged client, written at exit to a
 * second file, $INLAT_FRAMES with .in.txt replaced by .pt.txt (see
 * code/renderervk/README.md, Performance, Present timing):
 *
 * - X11 (Xwayland: SDL 2's default; SDL 3 with SDL_VIDEODRIVER=x11):
 *   Mesa's WSI gets a PresentCompleteNotify event for every present
 *   (xcb_wait_for_special_event in its event thread, wrapped here): its
 *   serial (the present's number on its swapchain), its mode (copy,
 *   flip, skip, suboptimal copy), and the UST and MSC the X server
 *   gives it; vkWaitForPresentKHR returns once it has arrived and the
 *   frame's GPU work is done. Xwayland sends it for an asynchronous
 *   flip (immediate, mailbox, FIFO relaxed) once the compositor has the
 *   commit, with the UST of the last frame callback, and for a FIFO
 *   flip at the compositor's next frame callback, which KWin sends
 *   after compositing the frame, before its page flip: neither is the
 *   time the frame reaches the display.
 * - Wayland (SDL 3's default on KWin; SDL 2 with
 *   SDL_VIDEODRIVER=wayland): Mesa's WSI asks for presentation feedback
 *   (wp_presentation) for presents that carry a present id; this
 *   library enables VK_KHR_present_id at vkCreateDevice and numbers
 *   every present, and wraps the feedback's listener
 *   (wl_proxy_add_listener): the compositor's time of the page flip
 *   that showed the frame (KWin: the kernel's timestamp,
 *   CLOCK_MONOTONIC; for a tearing flip the last vblank's) and its
 *   flags (vsync or not), or that it was discarded (replaced by a later
 *   present before any flip).
 * - GPU completion: every vkQueueSubmit also signals a timeline semaphore
 *   of this library with the next value (VK_KHR_timeline_semaphore enabled
 *   at vkCreateDevice), and a thread waits for it: when the GPU finished
 *   each submission, so when each frame's image was ready. (A thread
 *   calling vkWaitForPresentKHR would see that and the X11 completion
 *   together, but the swapchain is externally synchronized for it: waiting
 *   while the game acquires and presents is invalid, and it corrupted the
 *   heap with Wayland's WSI.)
 * - The display: a thread waits for every vblank of the active CRTC
 *   (DRM_IOCTL_WAIT_VBLANK on the card node, allowed without being DRM
 *   master) and records the kernel's timestamp (the start of scanout of the
 *   frame that follows) and the framebuffer on the primary plane then, with
 *   its format: one of the compositor's (KWin composites into 10-bit AR30
 *   buffers) or one of the game's swapchain images (direct scanout, the
 *   compositor bypassed). INLAT_FBPOLL_US=N also polls the primary plane
 *   every N us and records each change of framebuffer (KWin's commit: with
 *   tearing, the flip).
 * - Each swapchain: its present mode and whether the SDL window was
 *   fullscreen (SDL_CreateWindow and SDL 3's SDL_CreateWindowWithProperties
 *   wrapped).
 * - INLAT_PRESENT_MODE=immediate|mailbox|fifo|fifo_relaxed replaces the
 *   present mode of every swapchain the game creates (whatever
 *   r_swapInterval asked for).
 *
 * The .pt.txt file has one line per record, tagged by its first letter
 * (columns in its header); tools/framelog.py --display reads it with the
 * .in.txt and renderer_timed.so's files of perf.py --framelog runs and
 * works out when each frame reached the display.
 */

#define _GNU_SOURCE		// RTLD_NEXT
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MAX_FRAMES	1000000

typedef void ( *voidFunc_t )( void );
typedef voidFunc_t ( *getProcAddr_t )( void *handle, const char *name );	// vkGet{Instance,Device}ProcAddr

#define MAX_QUEUED	64		// frames handed to the renderer's submit thread, not presented yet

static long long		( *in_frames )[2];	// read, present
static int				in_numFrames;
static long long		in_read;			// first SDL_PollEvent since the last present (or frame queued), 0: none
static getProcAddr_t	in_getInstanceProcAddr, in_getDeviceProcAddr;
static PFN_vkQueuePresentKHR	in_queuePresent;

// the submit thread's frames: the read of each frame queued, by serial
static pthread_mutex_t	in_queueLock = PTHREAD_MUTEX_INITIALIZER;
static struct { unsigned long long serial; long long read; } in_queued[MAX_QUEUED];
static __thread int		in_threadPresent;	// this thread presents a queued frame
static __thread long long	in_threadRead;		// its read

static long long IN_Now( void ) {
	struct timespec ts;

	clock_gettime( CLOCK_MONOTONIC, &ts );
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void IN_Present( void ) {
	long long now = IN_Now(), read;

	if ( in_threadPresent ) {
		// a frame of the submit thread: the read of its frame (the main
		// thread's in_read is the next frame's)
		read = in_threadRead;
		in_threadPresent = 0;
	} else {
		read = in_read;
		in_read = 0;
	}
	if ( !in_frames ) {
		in_frames = malloc( sizeof( *in_frames ) * MAX_FRAMES );
	}
	if ( in_frames && in_numFrames < MAX_FRAMES ) {
		in_frames[in_numFrames][0] = read;
		in_frames[in_numFrames][1] = now;
		in_numFrames++;
	}
}

// the renderer's main thread hands a frame to its submit thread
void inlat_frame_queued( unsigned long long serial ) {
	pthread_mutex_lock( &in_queueLock );
	in_queued[serial % MAX_QUEUED].serial = serial;
	in_queued[serial % MAX_QUEUED].read = in_read;
	pthread_mutex_unlock( &in_queueLock );
	in_read = 0;
}

// the submit thread presents that frame next
void inlat_frame_presenting( unsigned long long serial ) {
	pthread_mutex_lock( &in_queueLock );
	in_threadRead = in_queued[serial % MAX_QUEUED].serial == serial ? in_queued[serial % MAX_QUEUED].read : 0;
	pthread_mutex_unlock( &in_queueLock );
	in_threadPresent = 1;
}

/*
==============================================================================

present timing (INLAT_DISPLAY=1)

==============================================================================
*/

#define MAX_RECORDS		1000000		// completions, feedback, waits
#define MAX_VBLANKS		400000
#define MAX_POLLS		400000
#define MAX_SWAPCHAINS	256
#define MAX_FBS			256
#define MAX_PENDING		256			// feedback objects in flight

typedef struct {
	int			image, swapchain;	// image index, swapchain (in order of creation)
	unsigned	serial;				// the present's number on its swapchain, from 1
	uint64_t	id;					// present id, 0: none
	uint64_t	gpu;				// the timeline value of the last submission before it, 0: none
} presentRecord_t;

typedef struct {
	VkSwapchainKHR		handle;
	VkPresentModeKHR	mode;
	unsigned			serial;		// presents so far
	uint32_t			eid;		// X11 Present event id, 0: none seen
	int					fullscreen;	// the SDL window's state when it was created
	uint32_t			width, height;
} swapchain_t;

typedef struct { long long recv; uint32_t eid, serial; int kind, mode; uint64_t ust, msc; } completeRecord_t;
typedef struct { int frame; long long recv, presented; uint32_t refresh, flags; } feedbackRecord_t;
typedef struct { unsigned seq; long long time, wake; uint32_t fb, format, overlay, overlayFormat; } vblankRecord_t;
typedef struct { long long time; uint32_t fb, format; } pollRecord_t;
typedef struct { uint32_t id, width, height, format; uint64_t modifier; } fbRecord_t;

static int					in_display = -1;	// INLAT_DISPLAY, -1: not read yet
static int					in_forceMode = -1;	// INLAT_PRESENT_MODE
static pthread_mutex_t		in_lock = PTHREAD_MUTEX_INITIALIZER;

static presentRecord_t		*in_presents;		// per frame of in_frames
static swapchain_t			in_swapchains[MAX_SWAPCHAINS];
static int					in_numSwapchains;
static __thread int			in_creating = -1;	// swapchain being created on this thread
static __thread int			in_presenting = -1;	// frame being presented on this thread

static completeRecord_t		*in_completes;
static int					in_numCompletes;
static feedbackRecord_t		*in_feedback;
static int					in_numFeedback;
static vblankRecord_t		*in_vblanks;
static int					in_numVblanks;
static pollRecord_t			*in_polls;
static int					in_numPolls;
static fbRecord_t			in_fbs[MAX_FBS];
static int					in_numFbs;
static char					in_displayInfo[256];

static VkInstance			in_instance;
static VkDevice				in_device;
static int					in_presentId;		// VK_KHR_present_id enabled
static int					in_gamePresentIds;	// the game enabled it and numbers its presents (the Vulkan renderer with VK_KHR_present_wait)
static uint64_t				in_nextId;
static PFN_vkCreateDevice				in_createDevice;
static PFN_vkCreateSwapchainKHR			in_createSwapchain;
static void								*in_window;		// the game's SDL window

static int IN_Display( void ) {
	if ( in_display < 0 ) {
		const char *s = getenv( "INLAT_DISPLAY" ), *m = getenv( "INLAT_PRESENT_MODE" );

		in_display = s && atoi( s ) > 0;
		if ( m && !strcmp( m, "immediate" ) ) {
			in_forceMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
		} else if ( m && !strcmp( m, "mailbox" ) ) {
			in_forceMode = VK_PRESENT_MODE_MAILBOX_KHR;
		} else if ( m && !strcmp( m, "fifo" ) ) {
			in_forceMode = VK_PRESENT_MODE_FIFO_KHR;
		} else if ( m && !strcmp( m, "fifo_relaxed" ) ) {
			in_forceMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
		}
		if ( in_display ) {
			in_presents = calloc( MAX_FRAMES, sizeof( *in_presents ) );
			in_completes = calloc( MAX_RECORDS, sizeof( *in_completes ) );
			in_feedback = calloc( MAX_RECORDS, sizeof( *in_feedback ) );
			in_vblanks = calloc( MAX_VBLANKS, sizeof( *in_vblanks ) );
			in_polls = calloc( MAX_POLLS, sizeof( *in_polls ) );
			if ( !in_presents || !in_completes || !in_feedback || !in_vblanks || !in_polls ) {
				in_display = 0;
			}
		}
	}
	return in_display;
}

// records are published by storing the count after the record (a single
// writer per array, or under in_lock); the writer at exit reads the count first
static void IN_Publish( int *count, int value ) {
	__atomic_store_n( count, value, __ATOMIC_RELEASE );
}

/*
------------------------------------------------------------------------------
the display: vblanks and the primary plane's framebuffer
------------------------------------------------------------------------------
*/

static int		in_drmFd = -1;
static int		in_pipe;
static uint32_t	in_primary, in_overlay;		// planes on the active CRTC

static uint32_t IN_PlaneFb( uint32_t plane ) {
	drmModePlane *p;
	uint32_t fb;

	if ( !plane || !( p = drmModeGetPlane( in_drmFd, plane ) ) ) {
		return 0;
	}
	fb = p->fb_id;
	drmModeFreePlane( p );
	return fb;
}

// the format of a framebuffer (fourcc, 0: none or unknown); ids are reused
// for new framebuffers, so it is read again whenever the plane's id changes
static uint32_t IN_FbFormat( uint32_t id, uint32_t *lastId, uint32_t *lastFormat ) {
	drmModeFB2 *fb;

	if ( id != *lastId ) {
		*lastId = id;
		*lastFormat = 0;
		if ( id && ( fb = drmModeGetFB2( in_drmFd, id ) ) ) {
			*lastFormat = fb->pixel_format;
			drmModeFreeFB2( fb );
		}
	}
	return *lastFormat;
}

// lists a framebuffer the first time it is seen (only the vblank thread calls it)
static void IN_NoteFb( uint32_t id ) {
	drmModeFB2 *fb;
	int i;

	if ( !id || in_numFbs >= MAX_FBS ) {
		return;
	}
	for ( i = 0; i < in_numFbs; i++ ) {
		if ( in_fbs[i].id == id ) {
			return;
		}
	}
	in_fbs[in_numFbs].id = id;
	if ( ( fb = drmModeGetFB2( in_drmFd, id ) ) ) {
		in_fbs[in_numFbs].width = fb->width;
		in_fbs[in_numFbs].height = fb->height;
		in_fbs[in_numFbs].format = fb->pixel_format;
		in_fbs[in_numFbs].modifier = fb->modifier;
		drmModeFreeFB2( fb );
	}
	IN_Publish( &in_numFbs, in_numFbs + 1 );
}

static int IN_PlaneType( uint32_t plane ) {
	drmModeObjectProperties *props = drmModeObjectGetProperties( in_drmFd, plane, DRM_MODE_OBJECT_PLANE );
	int type = -1;
	uint32_t i;

	for ( i = 0; props && i < props->count_props; i++ ) {
		drmModePropertyRes *prop = drmModeGetProperty( in_drmFd, props->props[i] );

		if ( prop && !strcmp( prop->name, "type" ) ) {
			type = (int)props->prop_values[i];
		}
		drmModeFreeProperty( prop );
	}
	drmModeFreeObjectProperties( props );
	return type;
}

// the first card with an active CRTC, its primary plane and an overlay plane on it
static int IN_OpenDisplay( void ) {
	char path[32];
	int card, i;

	for ( card = 0; card < 16; card++ ) {
		drmModeRes *res;
		drmModePlaneRes *planes;
		uint32_t crtc = 0;
		unsigned j;

		snprintf( path, sizeof( path ), "/dev/dri/card%d", card );
		if ( ( in_drmFd = open( path, O_RDWR | O_CLOEXEC ) ) < 0 ) {
			continue;
		}
		drmSetClientCap( in_drmFd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 );
		if ( ( res = drmModeGetResources( in_drmFd ) ) ) {
			for ( i = 0; i < res->count_crtcs && !crtc; i++ ) {
				drmModeCrtc *c = drmModeGetCrtc( in_drmFd, res->crtcs[i] );

				if ( c && c->mode_valid ) {
					crtc = c->crtc_id;
					in_pipe = i;
					snprintf( in_displayInfo, sizeof( in_displayInfo ), "%s crtc %u (pipe %d) %ux%u, pixel clock %u kHz, htotal %u, vtotal %u",
						path, crtc, i, c->mode.hdisplay, c->mode.vdisplay, c->mode.clock, c->mode.htotal, c->mode.vtotal );
				}
				drmModeFreeCrtc( c );
			}
			drmModeFreeResources( res );
		}
		if ( crtc && ( planes = drmModeGetPlaneResources( in_drmFd ) ) ) {
			for ( j = 0; j < planes->count_planes; j++ ) {
				drmModePlane *p = drmModeGetPlane( in_drmFd, planes->planes[j] );
				int type = p && p->crtc_id == crtc ? IN_PlaneType( p->plane_id ) : -1;

				if ( type == DRM_PLANE_TYPE_PRIMARY && !in_primary ) {
					in_primary = p->plane_id;
				} else if ( type == DRM_PLANE_TYPE_OVERLAY && !in_overlay ) {
					in_overlay = p->plane_id;
				}
				drmModeFreePlane( p );
			}
			drmModeFreePlaneResources( planes );
		}
		if ( crtc && in_primary ) {
			size_t len = strlen( in_displayInfo );

			snprintf( in_displayInfo + len, sizeof( in_displayInfo ) - len, ", primary plane %u, overlay plane %u", in_primary, in_overlay );
			return 1;
		}
		close( in_drmFd );
		in_drmFd = -1;
	}
	return 0;
}

static void *IN_VblankThread( void *arg ) {
	unsigned type = DRM_VBLANK_RELATIVE;
	uint32_t lastFb = 0, lastFormat = 0, lastOverlay = 0, lastOverlayFormat = 0;

	if ( in_pipe == 1 ) {
		type |= DRM_VBLANK_SECONDARY;
	} else if ( in_pipe > 1 ) {
		type |= ( in_pipe << DRM_VBLANK_HIGH_CRTC_SHIFT ) & DRM_VBLANK_HIGH_CRTC_MASK;
	}
	while ( in_numVblanks < MAX_VBLANKS ) {
		drmVBlank vb;
		vblankRecord_t *r = &in_vblanks[in_numVblanks];

		memset( &vb, 0, sizeof( vb ) );
		vb.request.type = type;
		vb.request.sequence = 1;
		if ( drmWaitVBlank( in_drmFd, &vb ) ) {
			usleep( 10000 );	// display off
			continue;
		}
		r->wake = IN_Now();
		r->seq = vb.reply.sequence;
		r->time = vb.reply.tval_sec * 1000000LL + vb.reply.tval_usec;
		r->fb = IN_PlaneFb( in_primary );
		r->format = IN_FbFormat( r->fb, &lastFb, &lastFormat );
		r->overlay = IN_PlaneFb( in_overlay );
		r->overlayFormat = IN_FbFormat( r->overlay, &lastOverlay, &lastOverlayFormat );
		IN_NoteFb( r->fb );
		IN_NoteFb( r->overlay );
		IN_Publish( &in_numVblanks, in_numVblanks + 1 );
	}
	return arg;
}

static void *IN_PollThread( void *arg ) {
	struct timespec ts;
	long interval = (long)arg * 1000;
	uint32_t last = 0, lastFb = 0, lastFormat = 0;

	clock_gettime( CLOCK_MONOTONIC, &ts );
	while ( in_numPolls < MAX_POLLS ) {
		uint32_t fb;

		ts.tv_nsec += interval;
		while ( ts.tv_nsec >= 1000000000 ) {
			ts.tv_nsec -= 1000000000;
			ts.tv_sec++;
		}
		clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL );
		fb = IN_PlaneFb( in_primary );
		if ( fb != last ) {
			in_polls[in_numPolls].time = IN_Now();
			in_polls[in_numPolls].fb = fb;
			in_polls[in_numPolls].format = IN_FbFormat( fb, &lastFb, &lastFormat );
			IN_Publish( &in_numPolls, in_numPolls + 1 );
			last = fb;
		}
	}
	return arg;
}

static void IN_StartDisplay( void ) {
	static int started;
	pthread_t thread;
	const char *poll = getenv( "INLAT_FBPOLL_US" );

	if ( started ) {
		return;
	}
	started = 1;
	if ( !IN_OpenDisplay() ) {
		snprintf( in_displayInfo, sizeof( in_displayInfo ), "no card with an active CRTC" );
		return;
	}
	pthread_create( &thread, NULL, IN_VblankThread, NULL );
	pthread_detach( thread );
	if ( poll && atol( poll ) > 0 ) {
		pthread_create( &thread, NULL, IN_PollThread, (void *)atol( poll ) );
		pthread_detach( thread );
	}
}

/*
------------------------------------------------------------------------------
GPU completion: a timeline semaphore of this library, signaled by every
submission with the next value, and a thread that waits for it (a host
wait on its own semaphore: nothing the game uses is touched)
------------------------------------------------------------------------------
*/

static VkSemaphore					in_timeline;
static uint64_t						in_submitValue;		// the last value signaled
static long long					*in_gpuDone;			// per value
static volatile int					in_stopGpu;
static pthread_t					in_gpuThread;
static PFN_vkQueueSubmit			in_queueSubmit;
static PFN_vkDestroyDevice			in_destroyDevice;
static PFN_vkWaitSemaphores			in_waitSemaphores;
static PFN_vkGetSemaphoreCounterValue	in_getCounter;
static PFN_vkDestroySemaphore		in_destroySemaphore;

static void *IN_GpuThread( void *arg ) {
	uint64_t next = (uint64_t)(uintptr_t)arg + 1;

	while ( !in_stopGpu ) {
		VkSemaphoreWaitInfo wait;
		uint64_t done = 0, v;

		memset( &wait, 0, sizeof( wait ) );
		wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
		wait.semaphoreCount = 1;
		wait.pSemaphores = &in_timeline;
		wait.pValues = &next;
		if ( in_waitSemaphores( in_device, &wait, 100000000 ) != VK_SUCCESS ) {	// 100 ms: nothing submitted
			continue;
		}
		if ( in_getCounter( in_device, in_timeline, &done ) != VK_SUCCESS ) {
			done = next;
		}
		// values that completed together get the same time
		for ( v = next; v <= done && v < MAX_RECORDS; v++ ) {
			in_gpuDone[v] = IN_Now();
		}
		next = done + 1;
	}
	return arg;
}

static void IN_StopGpu( void ) {
	if ( in_timeline ) {
		in_stopGpu = 1;
		pthread_join( in_gpuThread, NULL );
		in_destroySemaphore( in_device, in_timeline, NULL );
		in_timeline = VK_NULL_HANDLE;
		in_stopGpu = 0;
	}
}

static void IN_StartGpu( void ) {
	VkSemaphoreTypeCreateInfo type;
	VkSemaphoreCreateInfo ci;
	PFN_vkCreateSemaphore create = (PFN_vkCreateSemaphore)in_getDeviceProcAddr( in_device, "vkCreateSemaphore" );

	// the game's instance is Vulkan 1.1: VK_KHR_timeline_semaphore's entry points
	in_waitSemaphores = (PFN_vkWaitSemaphores)in_getDeviceProcAddr( in_device, "vkWaitSemaphoresKHR" );
	in_getCounter = (PFN_vkGetSemaphoreCounterValue)in_getDeviceProcAddr( in_device, "vkGetSemaphoreCounterValueKHR" );
	in_destroySemaphore = (PFN_vkDestroySemaphore)in_getDeviceProcAddr( in_device, "vkDestroySemaphore" );
	if ( !in_gpuDone ) {
		in_gpuDone = calloc( MAX_RECORDS, sizeof( *in_gpuDone ) );
	}
	if ( !create || !in_waitSemaphores || !in_getCounter || !in_destroySemaphore || !in_gpuDone ) {
		return;
	}
	memset( &type, 0, sizeof( type ) );
	type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	type.initialValue = in_submitValue;	// values go on after a vid_restart
	memset( &ci, 0, sizeof( ci ) );
	ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	ci.pNext = &type;
	if ( create( in_device, &ci, NULL, &in_timeline ) != VK_SUCCESS ) {
		in_timeline = VK_NULL_HANDLE;
		return;
	}
	if ( pthread_create( &in_gpuThread, NULL, IN_GpuThread, (void *)(uintptr_t)in_submitValue ) ) {
		in_destroySemaphore( in_device, in_timeline, NULL );
		in_timeline = VK_NULL_HANDLE;
	}
}

static VkResult VKAPI_CALL IN_QueueSubmit( VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence ) {
	VkSubmitInfo copy[8];
	VkTimelineSemaphoreSubmitInfo values;
	VkSemaphore signals[16];
	uint64_t signalValues[16];
	const VkBaseInStructure *next;
	VkSubmitInfo *last;
	uint32_t i;

	if ( !in_timeline || !count || count > 8 || submits[count - 1].signalSemaphoreCount >= 16 ) {
		return in_queueSubmit( queue, count, submits, fence );
	}
	for ( next = submits[count - 1].pNext; next; next = next->pNext ) {
		if ( next->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO ) {
			return in_queueSubmit( queue, count, submits, fence );
		}
	}
	memcpy( copy, submits, count * sizeof( *copy ) );
	last = &copy[count - 1];
	for ( i = 0; i < last->signalSemaphoreCount; i++ ) {
		signals[i] = last->pSignalSemaphores[i];
		signalValues[i] = 0;	// binary semaphores: ignored
	}
	signals[i] = in_timeline;
	signalValues[i] = in_submitValue + 1;
	memset( &values, 0, sizeof( values ) );
	values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	values.pNext = last->pNext;
	values.signalSemaphoreValueCount = last->signalSemaphoreCount + 1;
	values.pSignalSemaphoreValues = signalValues;
	last->pNext = &values;
	last->pSignalSemaphores = signals;
	last->signalSemaphoreCount++;
	in_submitValue++;	// one thread at a time submits and presents (the queue is externally synchronized)
	return in_queueSubmit( queue, count, copy, fence );
}

static void VKAPI_CALL IN_DestroyDevice( VkDevice device, const VkAllocationCallbacks *alloc ) {
	if ( device == in_device ) {
		IN_StopGpu();
	}
	in_destroyDevice( device, alloc );
}

/*
------------------------------------------------------------------------------
Vulkan: device, swapchains, presents
------------------------------------------------------------------------------
*/

static int IN_HasDeviceExtension( VkPhysicalDevice pd, const char *name ) {
	PFN_vkEnumerateDeviceExtensionProperties enumerate =
		(PFN_vkEnumerateDeviceExtensionProperties)in_getInstanceProcAddr( in_instance, "vkEnumerateDeviceExtensionProperties" );
	VkExtensionProperties *props;
	uint32_t n = 0, i;
	int found = 0;

	if ( !enumerate || enumerate( pd, NULL, &n, NULL ) != VK_SUCCESS || !( props = calloc( n + 1, sizeof( *props ) ) ) ) {
		return 0;
	}
	if ( enumerate( pd, NULL, &n, props ) == VK_SUCCESS ) {
		for ( i = 0; i < n; i++ ) {
			found |= !strcmp( props[i].extensionName, name );
		}
	}
	free( props );
	return found;
}

static VkResult VKAPI_CALL IN_CreateDevice( VkPhysicalDevice pd, const VkDeviceCreateInfo *info,
	const VkAllocationCallbacks *alloc, VkDevice *device ) {
	PFN_vkGetPhysicalDeviceFeatures2 features2 = (PFN_vkGetPhysicalDeviceFeatures2)in_getInstanceProcAddr( in_instance, "vkGetPhysicalDeviceFeatures2" );
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline, supported;
	VkPhysicalDevicePresentIdFeaturesKHR idFeatures;
	VkPhysicalDeviceFeatures2 query;
	VkDeviceCreateInfo ci;
	const char **names;
	VkResult result;
	int useTimeline = 0;

	if ( !IN_Display() || !in_instance || !in_getDeviceProcAddr ) {
		return in_createDevice( pd, info, alloc, device );
	}
	ci = *info;
	names = calloc( info->enabledExtensionCount + 2, sizeof( *names ) );
	memcpy( names, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof( *names ) );
	ci.ppEnabledExtensionNames = names;
	// VK_KHR_present_id: Wayland's WSI asks for presentation feedback only for presents with an id;
	// a game that enabled it numbers its presents itself, and those ids are used
	in_presentId = IN_HasDeviceExtension( pd, VK_KHR_PRESENT_ID_EXTENSION_NAME );
	in_gamePresentIds = 0;
	for ( uint32_t e = 0; e < info->enabledExtensionCount; e++ ) {
		if ( !strcmp( info->ppEnabledExtensionNames[e], VK_KHR_PRESENT_ID_EXTENSION_NAME ) ) {
			in_gamePresentIds = 1;
		}
	}
	if ( in_presentId && !in_gamePresentIds ) {
		names[ci.enabledExtensionCount++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
		memset( &idFeatures, 0, sizeof( idFeatures ) );
		idFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
		idFeatures.presentId = VK_TRUE;
		idFeatures.pNext = (void *)ci.pNext;
		ci.pNext = &idFeatures;
	}
	// timeline semaphores (VK_KHR_timeline_semaphore: the game's instance is
	// Vulkan 1.1) for the GPU completion of every submission
	memset( &supported, 0, sizeof( supported ) );
	supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
	if ( features2 ) {
		memset( &query, 0, sizeof( query ) );
		query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		query.pNext = &supported;
		features2( pd, &query );
		useTimeline = supported.timelineSemaphore && IN_HasDeviceExtension( pd, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME );
	}
	if ( useTimeline ) {
		names[ci.enabledExtensionCount++] = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
		memset( &timeline, 0, sizeof( timeline ) );
		timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
		timeline.timelineSemaphore = VK_TRUE;
		timeline.pNext = (void *)ci.pNext;
		ci.pNext = &timeline;
	}
	result = in_createDevice( pd, &ci, alloc, device );
	free( names );
	if ( result != VK_SUCCESS ) {
		in_presentId = 0;
		return in_createDevice( pd, info, alloc, device );
	}
	in_device = *device;
	if ( useTimeline ) {
		IN_StartGpu();
	}
	return result;
}

static VkResult VKAPI_CALL IN_CreateSwapchain( VkDevice device, const VkSwapchainCreateInfoKHR *info,
	const VkAllocationCallbacks *alloc, VkSwapchainKHR *swapchain ) {
	VkSwapchainCreateInfoKHR ci = *info;
	VkResult result;
	int n;

	if ( !IN_Display() ) {
		return in_createSwapchain( device, info, alloc, swapchain );
	}
	if ( in_forceMode >= 0 ) {
		ci.presentMode = (VkPresentModeKHR)in_forceMode;
	}
	pthread_mutex_lock( &in_lock );
	n = in_numSwapchains < MAX_SWAPCHAINS ? in_numSwapchains++ : MAX_SWAPCHAINS - 1;
	memset( &in_swapchains[n], 0, sizeof( in_swapchains[n] ) );
	in_swapchains[n].mode = ci.presentMode;
	in_swapchains[n].width = ci.imageExtent.width;
	in_swapchains[n].height = ci.imageExtent.height;
	if ( in_window ) {
		static uint32_t ( *getFlags )( void * );	// SDL 3's are 64-bit, the same low ones

		if ( !getFlags ) {
			getFlags = (uint32_t ( * )( void * ))dlsym( RTLD_NEXT, "SDL_GetWindowFlags" );
		}
		in_swapchains[n].fullscreen = getFlags && ( getFlags( in_window ) & 0x1 );	// SDL_WINDOW_FULLSCREEN
	}
	pthread_mutex_unlock( &in_lock );
	in_creating = n;	// the X11 WSI selects its Present events in here
	result = in_createSwapchain( device, &ci, alloc, swapchain );
	in_creating = -1;
	if ( result == VK_SUCCESS ) {
		in_swapchains[n].handle = *swapchain;
	}
	IN_StartDisplay();
	return result;
}

static VkResult VKAPI_CALL IN_QueuePresent( VkQueue queue, const VkPresentInfoKHR *presentInfo ) {
	VkPresentInfoKHR info;
	VkPresentIdKHR ids;
	presentRecord_t *r;
	uint64_t id = 0;
	VkResult result;
	int frame, i, sc = -1;

	IN_Present();
	if ( !IN_Display() || !in_numFrames || in_numFrames >= MAX_FRAMES ) {
		return in_queuePresent( queue, presentInfo );
	}
	frame = in_numFrames - 1;
	r = &in_presents[frame];
	r->gpu = in_timeline ? in_submitValue : 0;
	pthread_mutex_lock( &in_lock );
	for ( i = in_numSwapchains - 1; i >= 0 && presentInfo->swapchainCount; i-- ) {
		if ( in_swapchains[i].handle == presentInfo->pSwapchains[0] ) {
			sc = i;
			break;
		}
	}
	if ( sc >= 0 ) {
		r->swapchain = sc;
		r->serial = ++in_swapchains[sc].serial;
		r->image = presentInfo->pImageIndices[0];
	} else {
		r->swapchain = -1;
	}
	pthread_mutex_unlock( &in_lock );

	info = *presentInfo;
	if ( in_gamePresentIds ) {
		const VkBaseInStructure *next;

		for ( next = (const VkBaseInStructure *)presentInfo->pNext; next; next = next->pNext ) {
			if ( next->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR && presentInfo->swapchainCount == 1 ) {
				r->id = ( (const VkPresentIdKHR *)next )->pPresentIds[0];
			}
		}
	} else if ( in_presentId && presentInfo->swapchainCount == 1 ) {
		id = r->id = ++in_nextId;
		memset( &ids, 0, sizeof( ids ) );
		ids.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
		ids.pNext = info.pNext;
		ids.swapchainCount = 1;
		ids.pPresentIds = &id;
		info.pNext = &ids;
	}
	in_presenting = frame;	// Wayland's WSI creates the frame's feedback object in here
	result = in_queuePresent( queue, &info );
	in_presenting = -1;
	return result;
}

/*
------------------------------------------------------------------------------
X11: the Present events of Mesa's WSI
------------------------------------------------------------------------------
*/

// the real function of a library the WSI loaded: the Vulkan driver is
// dlopen'ed with RTLD_LOCAL, so its libraries aren't in RTLD_NEXT's scope
static void *IN_Real( const char *lib, const char *name ) {
	void *handle = dlopen( lib, RTLD_NOW | RTLD_NOLOAD );
	void *f;

	if ( !handle ) {
		handle = dlopen( lib, RTLD_NOW );
	}
	f = handle ? dlsym( handle, name ) : NULL;
	return f ? f : dlsym( RTLD_NEXT, name );
}

typedef struct { unsigned int sequence; } xcbCookie_t;

static void IN_X11Event( const uint8_t *ev ) {
	completeRecord_t *r;
	uint16_t type;

	// a GE event of the Present extension (the special event queue has nothing else)
	if ( ( ev[0] & 0x7f ) != 35 ) {
		return;
	}
	memcpy( &type, ev + 8, 2 );
	if ( type != 1 ) {	// XCB_PRESENT_EVENT_COMPLETE_NOTIFY
		return;
	}
	pthread_mutex_lock( &in_lock );
	if ( in_numCompletes < MAX_RECORDS ) {
		r = &in_completes[in_numCompletes];
		r->recv = IN_Now();
		r->kind = ev[10];
		r->mode = ev[11];
		memcpy( &r->eid, ev + 12, 4 );
		memcpy( &r->serial, ev + 20, 4 );
		memcpy( &r->ust, ev + 24, 8 );
		// xcb inserts full_sequence (4 bytes) at offset 32 of GE events: the
		// wire's msc at 32 is read at 36
		memcpy( &r->msc, ev + 36, 8 );
		IN_Publish( &in_numCompletes, in_numCompletes + 1 );
	}
	pthread_mutex_unlock( &in_lock );
}

void *xcb_wait_for_special_event( void *c, void *se ) {
	static void *( *real )( void *, void * );
	void *ev;

	if ( !real ) {
		real = (void *( * )( void *, void * ))IN_Real( "libxcb.so.1", "xcb_wait_for_special_event" );
	}
	ev = real( c, se );
	if ( ev && IN_Display() ) {
		IN_X11Event( ev );
	}
	return ev;
}

void *xcb_poll_for_special_event( void *c, void *se ) {
	static void *( *real )( void *, void * );
	void *ev;

	if ( !real ) {
		real = (void *( * )( void *, void * ))IN_Real( "libxcb.so.1", "xcb_poll_for_special_event" );
	}
	ev = real( c, se );
	if ( ev && IN_Display() ) {
		IN_X11Event( ev );
	}
	return ev;
}

// the event id of a swapchain's Present events
static void IN_SelectInput( uint32_t eid ) {
	if ( in_creating >= 0 && IN_Display() ) {
		in_swapchains[in_creating].eid = eid;
	}
}

xcbCookie_t xcb_present_select_input_checked( void *c, uint32_t eid, uint32_t window, uint32_t mask ) {
	static xcbCookie_t ( *real )( void *, uint32_t, uint32_t, uint32_t );

	if ( !real ) {
		real = (xcbCookie_t ( * )( void *, uint32_t, uint32_t, uint32_t ))IN_Real( "libxcb-present.so.0", "xcb_present_select_input_checked" );
	}
	IN_SelectInput( eid );
	return real( c, eid, window, mask );
}

xcbCookie_t xcb_present_select_input( void *c, uint32_t eid, uint32_t window, uint32_t mask ) {
	static xcbCookie_t ( *real )( void *, uint32_t, uint32_t, uint32_t );

	if ( !real ) {
		real = (xcbCookie_t ( * )( void *, uint32_t, uint32_t, uint32_t ))IN_Real( "libxcb-present.so.0", "xcb_present_select_input" );
	}
	IN_SelectInput( eid );
	return real( c, eid, window, mask );
}

/*
------------------------------------------------------------------------------
Wayland: presentation feedback of the presents
------------------------------------------------------------------------------
*/

typedef void ( *listenerFunc_t )( void );

static struct { void *proxy; const listenerFunc_t *impl; int frame; } in_feedbackObjects[MAX_PENDING];

// takes the feedback object out of the table (the original listener destroys it)
static int IN_TakeFeedback( void *proxy, const listenerFunc_t **impl, int remove ) {
	int i, frame = -1;

	pthread_mutex_lock( &in_lock );
	for ( i = 0; i < MAX_PENDING; i++ ) {
		if ( in_feedbackObjects[i].proxy == proxy ) {
			*impl = in_feedbackObjects[i].impl;
			frame = in_feedbackObjects[i].frame;
			if ( remove ) {
				in_feedbackObjects[i].proxy = NULL;
			}
			break;
		}
	}
	pthread_mutex_unlock( &in_lock );
	return frame;
}

static void IN_AddFeedback( int frame, long long presented, uint32_t refresh, uint32_t flags ) {
	pthread_mutex_lock( &in_lock );
	if ( in_numFeedback < MAX_RECORDS ) {
		feedbackRecord_t *r = &in_feedback[in_numFeedback];

		r->frame = frame;
		r->recv = IN_Now();
		r->presented = presented;
		r->refresh = refresh;
		r->flags = flags;
		IN_Publish( &in_numFeedback, in_numFeedback + 1 );
	}
	pthread_mutex_unlock( &in_lock );
}

static void IN_FeedbackSyncOutput( void *data, void *proxy, void *output ) {
	const listenerFunc_t *impl = NULL;

	IN_TakeFeedback( proxy, &impl, 0 );
	if ( impl && impl[0] ) {
		( (void ( * )( void *, void *, void * ))impl[0] )( data, proxy, output );
	}
}

static void IN_FeedbackPresented( void *data, void *proxy, uint32_t secHi, uint32_t secLo, uint32_t nsec,
	uint32_t refresh, uint32_t seqHi, uint32_t seqLo, uint32_t flags ) {
	const listenerFunc_t *impl = NULL;
	int frame = IN_TakeFeedback( proxy, &impl, 1 );

	IN_AddFeedback( frame, ( ( (long long)secHi << 32 ) + secLo ) * 1000000LL + nsec / 1000, refresh, flags );
	if ( impl && impl[1] ) {
		( (void ( * )( void *, void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t ))impl[1] )(
			data, proxy, secHi, secLo, nsec, refresh, seqHi, seqLo, flags );
	}
}

static void IN_FeedbackDiscarded( void *data, void *proxy ) {
	const listenerFunc_t *impl = NULL;
	int frame = IN_TakeFeedback( proxy, &impl, 1 );

	IN_AddFeedback( frame, -1, 0, 0 );
	if ( impl && impl[2] ) {
		( (void ( * )( void *, void * ))impl[2] )( data, proxy );
	}
}

static const listenerFunc_t in_feedbackListener[3] = {
	(listenerFunc_t)IN_FeedbackSyncOutput, (listenerFunc_t)IN_FeedbackPresented, (listenerFunc_t)IN_FeedbackDiscarded
};

int wl_proxy_add_listener( void *proxy, const listenerFunc_t *impl, void *data ) {
	static int ( *real )( void *, const listenerFunc_t *, void * );
	static const char *( *getClass )( void * );
	int i;

	if ( !real ) {
		real = (int ( * )( void *, const listenerFunc_t *, void * ))IN_Real( "libwayland-client.so.0", "wl_proxy_add_listener" );
		getClass = (const char *( * )( void * ))IN_Real( "libwayland-client.so.0", "wl_proxy_get_class" );
	}
	if ( in_presenting >= 0 && getClass && !strcmp( getClass( proxy ), "wp_presentation_feedback" ) ) {
		pthread_mutex_lock( &in_lock );
		for ( i = 0; i < MAX_PENDING; i++ ) {
			if ( !in_feedbackObjects[i].proxy ) {
				in_feedbackObjects[i].proxy = proxy;
				in_feedbackObjects[i].impl = impl;
				in_feedbackObjects[i].frame = in_presenting;
				break;
			}
		}
		pthread_mutex_unlock( &in_lock );
		if ( i < MAX_PENDING ) {
			return real( proxy, in_feedbackListener, data );
		}
	}
	return real( proxy, impl, data );
}

/*
------------------------------------------------------------------------------
files
------------------------------------------------------------------------------
*/

static const char *IN_ModeName( int mode ) {
	switch ( mode ) {
	case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
	case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
	case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
	case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
	default: return "-";
	}
}

static const char *IN_Fourcc( uint32_t format ) {
	static char s[8][5];
	static int n;
	char *c = s[n++ & 7];

	if ( !format ) {
		return "-";
	}
	memcpy( c, &format, 4 );
	c[4] = 0;
	return c;
}

static void IN_WriteTiming( const char *framesPath, int numFrames ) {
	char path[1024];
	size_t len = strlen( framesPath );
	int nc = __atomic_load_n( &in_numCompletes, __ATOMIC_ACQUIRE ), nf = __atomic_load_n( &in_numFeedback, __ATOMIC_ACQUIRE );
	int nv = __atomic_load_n( &in_numVblanks, __ATOMIC_ACQUIRE );
	int np = __atomic_load_n( &in_numPolls, __ATOMIC_ACQUIRE ), nb = __atomic_load_n( &in_numFbs, __ATOMIC_ACQUIRE );
	FILE *f;
	int i;

	if ( len > 7 && !strcmp( framesPath + len - 7, ".in.txt" ) ) {
		snprintf( path, sizeof( path ), "%.*s.pt.txt", (int)( len - 7 ), framesPath );
	} else {
		snprintf( path, sizeof( path ), "%s.pt.txt", framesPath );
	}
	if ( !( f = fopen( path, "w" ) ) ) {
		return;
	}
	fprintf( f, "# inlat present timing 1: %d frames, %d swapchains, %d completions, %d feedback, %d vblanks, %d polls, %llu submissions"
		" (code/renderervk/tools/rtimer/inlat.c)\n", numFrames, in_numSwapchains, nc, nf, nv, np, (unsigned long long)in_submitValue );
	fprintf( f, "# display: %s\n", in_displayInfo );
	fprintf( f, "# present id %s, GPU completion %s, present mode %s\n", in_presentId ? "on" : "off",
		in_timeline || in_submitValue ? "on" : "off", in_forceMode >= 0 ? IN_ModeName( in_forceMode ) : "as asked" );
	fprintf( f, "# CLOCK_MONOTONIC microseconds; frames are the lines of the .in.txt file\n" );
	fprintf( f, "# F frame image swapchain serial id gpu: each present (swapchain -1: unknown; gpu: its last submission)\n" );
	fprintf( f, "# S swapchain mode eid fullscreen width height: each swapchain created, its present mode, X11 Present event id, the SDL window's fullscreen flag\n" );
	fprintf( f, "# C recv eid serial kind mode ust msc: X11 PresentCompleteNotify (mode 0 copy, 1 flip, 2 skip, 3 suboptimal copy)\n" );
	fprintf( f, "# P frame recv presented refresh flags: Wayland presentation feedback (presented -1: discarded; refresh ns)\n" );
	fprintf( f, "# V seq time wake fb format overlay format: DRM vblank (time: the kernel's timestamp), framebuffers on the primary and an overlay plane\n" );
	fprintf( f, "# B fb width height format modifier: each framebuffer id the first time it is seen (ids are reused)\n" );
	fprintf( f, "# G submission done: when the GPU completed each submission\n" );
	fprintf( f, "# U time fb format: primary plane framebuffer changes (INLAT_FBPOLL_US)\n" );
	for ( i = 0; i < numFrames; i++ ) {
		const presentRecord_t *r = &in_presents[i];

		if ( r->serial ) {
			fprintf( f, "F %d %d %d %u %llu %llu\n", i, r->image, r->swapchain, r->serial, (unsigned long long)r->id,
				(unsigned long long)r->gpu );
		}
	}
	for ( i = 0; i < in_numSwapchains; i++ ) {
		fprintf( f, "S %d %s %u %d %u %u\n", i, IN_ModeName( in_swapchains[i].mode ), in_swapchains[i].eid,
			in_swapchains[i].fullscreen, in_swapchains[i].width, in_swapchains[i].height );
	}
	for ( i = 0; i < nc; i++ ) {
		const completeRecord_t *r = &in_completes[i];

		fprintf( f, "C %lld %u %u %d %d %llu %llu\n", r->recv, r->eid, r->serial, r->kind, r->mode,
			(unsigned long long)r->ust, (unsigned long long)r->msc );
	}
	for ( i = 0; i < nf; i++ ) {
		fprintf( f, "P %d %lld %lld %u %u\n", in_feedback[i].frame, in_feedback[i].recv, in_feedback[i].presented,
			in_feedback[i].refresh, in_feedback[i].flags );
	}
	for ( i = 0; i < nv; i++ ) {
		fprintf( f, "V %u %lld %lld %u %s %u %s\n", in_vblanks[i].seq, in_vblanks[i].time, in_vblanks[i].wake,
			in_vblanks[i].fb, IN_Fourcc( in_vblanks[i].format ), in_vblanks[i].overlay, IN_Fourcc( in_vblanks[i].overlayFormat ) );
	}
	for ( i = 0; i < nb; i++ ) {
		fprintf( f, "B %u %u %u %s %llu\n", in_fbs[i].id, in_fbs[i].width, in_fbs[i].height,
			IN_Fourcc( in_fbs[i].format ), (unsigned long long)in_fbs[i].modifier );
	}
	for ( i = 1; in_gpuDone && i < MAX_RECORDS && (uint64_t)i <= in_submitValue; i++ ) {
		if ( in_gpuDone[i] ) {
			fprintf( f, "G %d %lld\n", i, in_gpuDone[i] );
		}
	}
	for ( i = 0; i < np; i++ ) {
		fprintf( f, "U %lld %u %s\n", in_polls[i].time, in_polls[i].fb, IN_Fourcc( in_polls[i].format ) );
	}
	fclose( f );
}

__attribute__(( destructor )) static void IN_Write( void ) {
	const char *path = getenv( "INLAT_FRAMES" );
	int n = in_numFrames;
	FILE *f;
	int i;

	if ( !path || !*path || !n || !( f = fopen( path, "w" ) ) ) {
		return;
	}
	fprintf( f, "# inlat frames 1: %d frames (code/renderervk/tools/rtimer/inlat.c)\n", n );
	fprintf( f, "# columns, CLOCK_MONOTONIC microseconds: frame read present\n" );
	for ( i = 0; i < n; i++ ) {
		fprintf( f, "%d %lld %lld\n", i, in_frames[i][0], in_frames[i][1] );
	}
	fclose( f );
	if ( in_display > 0 ) {
		IN_WriteTiming( path, n );
	}
}

/*
------------------------------------------------------------------------------
the entry points the game calls
------------------------------------------------------------------------------
*/

// SDL 3 (it has SDL_CreateWindowWithProperties) or SDL 2
static int IN_SDL3( void ) {
	static int sdl3 = -1;

	if ( sdl3 < 0 ) {
		sdl3 = dlsym( RTLD_NEXT, "SDL_CreateWindowWithProperties" ) != NULL;
	}
	return sdl3;
}

// SDL 3 returns a bool, SDL 2 an int that is 0 or 1: both in the low byte,
// which is what an SDL 3 caller reads and an SDL 2 caller gets as 0 or 1
int SDL_PollEvent( void *event ) {
	static unsigned char ( *real )( void * );

	if ( !real ) {
		real = (unsigned char ( * )( void * ))dlsym( RTLD_NEXT, "SDL_PollEvent" );
	}
	if ( !in_read ) {
		in_read = IN_Now();
	}
	return real( event ) ? 1 : 0;
}

// SDL 2: ( title, x, y, w, h, flags ), SDL 3: ( title, w, h, flags ) with
// 64-bit flags, which land in the fourth argument's register (x86-64)
void *SDL_CreateWindow( const char *title, int a, int b, uint64_t c, int d, uint32_t e ) {
	static void *real;

	if ( !real ) {
		real = dlsym( RTLD_NEXT, "SDL_CreateWindow" );
	}
	if ( IN_SDL3() ) {
		in_window = ( (void *( * )( const char *, int, int, uint64_t ))real )( title, a, b, c );
	} else {
		in_window = ( (void *( * )( const char *, int, int, int, int, uint32_t ))real )( title, a, b, (int)c, d, e );
	}
	return in_window;
}

void *SDL_CreateWindowWithProperties( uint32_t props ) {
	static void *( *real )( uint32_t );

	if ( !real ) {
		real = (void *( * )( uint32_t ))dlsym( RTLD_NEXT, "SDL_CreateWindowWithProperties" );
	}
	in_window = real( props );
	return in_window;
}

static voidFunc_t IN_WrapDeviceFunction( const char *name, voidFunc_t f ) {
	if ( !f ) {
		return f;
	}
	if ( !strcmp( name, "vkQueuePresentKHR" ) ) {
		in_queuePresent = (PFN_vkQueuePresentKHR)f;
		return (voidFunc_t)IN_QueuePresent;
	}
	if ( !strcmp( name, "vkCreateSwapchainKHR" ) ) {
		in_createSwapchain = (PFN_vkCreateSwapchainKHR)f;
		return (voidFunc_t)IN_CreateSwapchain;
	}
	if ( !strcmp( name, "vkQueueSubmit" ) ) {
		in_queueSubmit = (PFN_vkQueueSubmit)f;
		return (voidFunc_t)IN_QueueSubmit;
	}
	if ( !strcmp( name, "vkDestroyDevice" ) ) {
		in_destroyDevice = (PFN_vkDestroyDevice)f;
		return (voidFunc_t)IN_DestroyDevice;
	}
	return f;
}

static voidFunc_t IN_GetDeviceProcAddr( void *device, const char *name ) {
	return IN_WrapDeviceFunction( name, in_getDeviceProcAddr( device, name ) );
}

static voidFunc_t IN_GetInstanceProcAddr( void *instance, const char *name ) {
	voidFunc_t f = in_getInstanceProcAddr( instance, name );

	if ( instance ) {
		in_instance = (VkInstance)instance;
	}
	if ( f && !strcmp( name, "vkGetDeviceProcAddr" ) ) {
		in_getDeviceProcAddr = (getProcAddr_t)f;
		return (voidFunc_t)IN_GetDeviceProcAddr;
	}
	if ( f && !strcmp( name, "vkCreateDevice" ) ) {
		in_createDevice = (PFN_vkCreateDevice)f;
		return (voidFunc_t)IN_CreateDevice;
	}
	return IN_WrapDeviceFunction( name, f );
}

void *SDL_Vulkan_GetVkGetInstanceProcAddr( void ) {
	static void *( *real )( void );

	if ( !real ) {
		real = (void *( * )( void ))dlsym( RTLD_NEXT, "SDL_Vulkan_GetVkGetInstanceProcAddr" );
	}
	in_getInstanceProcAddr = (getProcAddr_t)real();
	return in_getInstanceProcAddr ? (void *)IN_GetInstanceProcAddr : NULL;
}
