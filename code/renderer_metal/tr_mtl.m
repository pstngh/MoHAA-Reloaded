/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA Metal port contributors

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
// tr_mtl.m -- Metal backend behind tr_mtl.h (Objective-C, ARC).
//
// Frame model: the scene is rendered into an offscreen color target + depth/stencil target, in one render
// pass per frame where possible. The command buffer and the pass begin lazily at the first draw or clear,
// wherever in the frame that happens (2D exports draw outside the render-command queue). mbEndFrame ends the
// pass, runs one fullscreen triangle that copies the scene into the drawable (gamma ramp applied in the
// shader), presents and commits. Only then does it wait for a free frame slot, so that wait falls before the
// engine's frame limiter and the next frame's input sampling: nothing waits for the GPU between input
// sampling and present.
//
// Drawables: in a composited window the WindowServer holds the layer's drawables until its next refresh, and
// nextDrawable blocked the frame for 1.3-1.9 ms (2-3 drawables, timedemo). A blocking present would cap the
// engine loop below com_maxfps and change usercmd timing, which GL1 (non-blocking swap) doesn't. So with vsync
// off a helper thread waits in nextDrawable and keeps one drawable ready: the frame takes it without waiting.
// When none is ready (direct to display: the last frame's flip is late, e.g. while the WindowServer is busy with
// another application's frame), the frame leaves its final pass to that thread, which presents it as soon as a
// drawable comes back, if it can still be on screen before the next frame (a late present, r_metalLatePresent);
// otherwise the frame skips its present. With vsync on (r_swapInterval 1) the next drawable is acquired at the end
// of the frame, blocking like GL1's swap.
//
// Waking the GPU: after ~1.5 ms without work the GPU powers down, and a commit then waits ~0.3-0.5 ms for it to
// start. The engine calls mbWakeGPU when a frame starts (right after its frame limiter), and a tiny command buffer
// has the GPU up when that frame commits, ~0.3-0.6 ms later (r_metalWakeGPU).
//
// GL state: the front end drives the GL1 fixed-function state model (tr_mtl.h). The setters only record
// state and set dirty bits; MB_PrepareDraw turns it into Metal state at draw time: a render pipeline from a
// cache keyed by blend factors, color mask, sampled texture units and alpha test (one generic stage shader,
// tr_mtl_stage.metal), a depth/stencil state from a cache keyed by the depth/stencil bits, samplers cached by
// their GL parameters and stored in each texture, and per-draw uniforms. Nothing is looked up by string and
// nothing is allocated per draw or per frame in the steady state.
//
// Vertices: client arrays are read at draw time and copied as they are into the frame slot's shared buffer
// ring (the shader fetches them by offset and stride). Inside mbLockArraysEXT the arrays that were enabled at
// the lock are copied once for all draws (the compiled-vertex-array contract); arrays enabled later are
// copied per draw. Immediate mode (mbBegin/mbEnd) collects vertices and converts quads/polygons/line loops.
//
// Textures: names index a texture table. A write to a texture that no frame in flight uses goes straight
// to its memory (replaceRegion). A write to a texture that a frame in flight (or the frame being recorded)
// still reads gives the name a fresh texture: the old contents are copied on the GPU and the new data is
// blitted, in an upload command buffer committed right before the frame's. Draws recorded before the write
// keep the old texture, as in GL. Old textures are released, or reused for the next such write, only after
// the frames that used them have completed.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>

#include "../renderercommon/tr_common.h"
#include "tr_mtl.h"
#include "tr_mtl_metallib.h"

qboolean	mbMultitexture;

#define MB_MAX_FRAMES		3
#define MB_RING_SIZE		( 8 * 1024 * 1024 )		// per frame slot to start with; grows if a frame needs more
#define MB_RING_ALIGN		16
#define MB_MAX_UNITS		2
#define MB_MATRIX_STACK		16
#define MB_TEXTURE_POOL		16						// replaced in-use textures kept for reuse
#define MB_NAME_REUSE_MIN	1024					// freed texture names wait in a queue this long before reuse
#define MB_PIPELINE_SLOTS	1024					// power of two
#define MB_DS_SLOTS			128
#define MB_SAMPLER_SLOTS	256

// stream a value that ARC keeps in a void * (retained) back as an object
#define MB_OBJ( p )			( (__bridge id)( p ) )
#define MB_RELEASE( p )		do { if ( p ) { (void)(__bridge_transfer id)( p ); ( p ) = NULL; } } while ( 0 )

static id<MTLDevice>				mb_device;
static id<MTLCommandQueue>			mb_queue;
static id<MTLLibrary>				mb_library;
static id<MTLRenderPipelineState>	mb_finalPipeline;
static id<MTLFunction>				mb_stageVS;
static id<MTLFunction>				mb_stageFS[8];		// [texMask | alphaTest << 2]
static id<MTLFunction>				mb_clearVS, mb_clearFS;
static CAMetalLayer					*mb_layer;
static id<MTLTexture>				mb_sceneColor;		// offscreen RGBA8, what the front end renders into
static id<MTLTexture>				mb_sceneDepth;		// Depth32Float_Stencil8
static id<MTLCommandBuffer>			mb_lastCommitted;	// for shutdown
static id<MTLSharedEvent>			mb_frameEvent;		// the GPU signals each frame's serial when its work is done

// drawable acquisition (see the top of the file)
static qboolean						mb_vsync;
static id<CAMetalDrawable>			mb_nextDrawable;	// vsync: acquired at the end of the previous frame
static pthread_t					mb_acquireThread;	// no vsync: keeps mb_readyDrawable filled
static qboolean						mb_acquireThreadRunning;
static _Atomic qboolean				mb_acquireQuit;
static dispatch_semaphore_t			mb_acquireRequest;	// signaled when mb_readyDrawable was taken
static os_unfair_lock				mb_readyLock = OS_UNFAIR_LOCK_INIT;
static id<CAMetalDrawable>			mb_readyDrawable;	// guarded by mb_readyLock

// late presents (vsync off): a frame that found no drawable ready leaves its final pass to the drawable thread, which
// encodes and presents it as soon as a drawable comes back, unless the next frame ends first (guarded by mb_readyLock)
static qboolean						mb_latePresent;		// r_metalLatePresent
static id<MTLCommandBuffer>			mb_lateCmd;			// enqueued right after the frame's command buffer; nil = none
static id<MTLTexture>				mb_lateScene;
static uint8_t						mb_lateRamp[256][4];
static uint64_t						mb_lateLatency;		// the frame's cl_latencyStats record
static uint64_t						mb_lateDeadline;	// mach time: commit it by then, or leave the drawable to the next frame
static uint64_t						mb_frameEnd, mb_frameInterval;	// mach time: the last frame's end, the smoothed frame interval
static uint64_t						mb_lateMargin;		// mach time: a late present's commit to its drawable's return (~2 ms)
static uint64_t						mb_lastSubmit;		// mach time: the last frame (or wake-up) committed
static uint64_t						mb_wakeIdle;		// mach time: no wake-up within this of the last commit (1 ms)
static id<MTLBuffer>				mb_wakeBuffer;		// what the wake-up's blit writes
static void MB_CancelLatePresent( void );

static cvar_t	*r_metalFramesInFlight;
static cvar_t	*r_metalPresentStats;
static cvar_t	*r_metalLatePresent;
static cvar_t	*r_metalPipelineCache;
static cvar_t	*r_metalWakeGPU;

// r_metalPresentStats: drawables actually shown on screen (presentedTime > 0) vs. presented
static _Atomic int	mb_statPresented, mb_statShown;
static int			mb_statFrames, mb_statSkipped;			// frames, frames without a ready drawable
static _Atomic int	mb_statLate;							// of those, presented late by the drawable thread
static uint64_t		mb_statDrawableWait, mb_statSlotWait;	// main thread mach ticks in nextDrawable / the frame slot wait
static uint64_t		mb_statDraws, mb_statPasses, mb_statClearQuads, mb_statOrphans, mb_statRingBytes;
static _Atomic uint64_t	mb_statGPUTime, mb_statGPULatency, mb_statCompletion;	// ns: frame command buffer GPU start..end, commit..GPU start, commit..completed handler
static _Atomic int		mb_statGPUFrames;
static int				mb_statWakes;			// mbWakeGPU's command buffers
static int			mb_statPipelines, mb_statRingGrowths;
static uint64_t		mb_statPipelineTime, mb_statPipelineMax;		// mach ticks creating pipelines (total, slowest)
static int			mb_statPipelinesCached;						// of mb_statPipelines, found in the pipeline cache
static int			mb_statPipelinesAhead, mb_statPipelinesAtDraw;	// compiled on Metal's threads ahead of their first draw, created at it

// cl_latencyStats (mbLatencyTag/mbLatencyPoll): when each tagged frame was committed, finished on the GPU and
// shown. Records are polled in frame order; the command buffer's completed handler and the drawable's presented
// handler (Metal and Core Animation threads) fill in their times under mb_latencyLock. A record is reused only
// after it was polled, and a handler only writes the record whose sequence number it holds, so a late handler
// never touches another frame's record.
#define MB_LATENCY_FRAMES	256
#define MB_LATENCY_GIVEUP	1000000		// us after the commit: a handler that never ran counts as no time

#define MB_LAT_COMMIT		1			// mbLatencyFrame_t.pending: times still to come
#define MB_LAT_GPU			2
#define MB_LAT_PRESENT		4

typedef struct {
	uint64_t	seq;					// 1-based
	int			tag;
	int			pending;				// MB_LAT_* bits
	long long	commitUsec, gpuDoneUsec;
	long long	presentUsec;			// > 0 shown at, 0 not presented (no drawable), -1 presented but never shown
} mbLatencyFrame_t;

static os_unfair_lock		mb_latencyLock = OS_UNFAIR_LOCK_INIT;
static mbLatencyFrame_t		mb_latency[MB_LATENCY_FRAMES];	// guarded by mb_latencyLock
static uint64_t				mb_latencyHead, mb_latencyTail;	// records made, records polled
static qboolean				mb_latencyTagged;				// mbLatencyTag was called for the frame being recorded
static int					mb_latencyTag;

static int		mb_framesInFlight;
static int		mb_width, mb_height;
static char		mb_rendererString[MAX_STRING_CHARS];

static uint8_t	mb_gammaRamp[256][4];					// .rgb per input value, see tr_mtl_final.metal

/*
=============================================================

FRAME STATE

=============================================================
*/

static uint64_t				mb_frameSerial = 1;			// the frame being recorded
static int					mb_frameSlot;				// mb_frameSerial % mb_framesInFlight

// per-slot buffer ring (shared memory: the CPU writes, the GPU reads)
static id<MTLBuffer>		mb_ringBuffers[MB_MAX_FRAMES];
static id<MTLBuffer>		mb_ring;					// == mb_ringBuffers[mb_frameSlot]
static byte					*mb_ringBase;
static NSUInteger			mb_ringSize, mb_ringUsed;
static unsigned				mb_ringGeneration;			// bumped when a slot's buffer is replaced (offsets go stale)

static id<MTLCommandBuffer>			mb_cmd;				// this frame's command buffer, begun lazily
static id<MTLRenderCommandEncoder>	mb_enc;				// the scene pass, begun lazily
static id<MTLCommandBuffer>			mb_uploadCmd;		// texture copies for this frame, committed before mb_cmd
static id<MTLBlitCommandEncoder>	mb_uploadBlit;

// what the next scene pass must do with its attachments
static unsigned		mb_pendingClear;					// MB_*_BUFFER_BIT folded into the pass load actions
static float		mb_pendingClearColor[4];
static double		mb_pendingClearDepth;
static int			mb_pendingClearStencil;
static qboolean		mb_sceneColorValid;					// the scene target holds an image (else clear it)
static qboolean		mb_depthValid;						// depth/stencil were stored by an interrupted pass

// objects the GPU may still use, released once the frame event reaches their serial
typedef struct {
	void		*object;
	uint64_t	serial;
} mbRetired_t;

static mbRetired_t	*mb_retired;
static int			mb_numRetired, mb_maxRetired;

// Readbacks (mbReadPixelsAsync): a blit into a shared buffer at that point of the frame, handed to the callback
// once the frame event has reached the frame's serial.
#define MB_MAX_READBACKS	16

typedef struct {
	void				*buffer;		// id<MTLBuffer>, retained
	uint64_t			serial;			// the frame that fills it
	int					width, height;
	int					format;			// MB_RGB, MB_RGBA or MB_STENCIL_INDEX
	mbReadbackFunc_t	done;
	void				*arg;
} mbReadback_t;

static mbReadback_t	mb_readbacks[MB_MAX_READBACKS];
static int			mb_numReadbacks;
static byte			*mb_readbackPixels;					// the callback's pixels, converted from the buffer
static size_t		mb_readbackPixelsSize;

// Depth queries (mbDepthQuery): one point per query, counted into the frame slot's visibility result buffer.
// The counts are copied out when the slot wait at the end of a frame finds that frame complete (so a query is
// answered r_metalFramesInFlight frames after it was issued) and stay readable for MB_QUERY_HISTORY frames.
#define MB_MAX_QUERIES		256
#define MB_QUERY_HISTORY	4
#define MB_QUERY_KEEP		64				// frames a pass still gets the visibility buffer after the last query
#define MB_QUERY_TAG( s )	( (int)( ( s ) % 0x7fffff ) + 1 )	// a frame serial as it is stored in a handle

typedef struct {
	uint64_t	serial;						// 0 = empty
	int			count;
	byte		passed[MB_MAX_QUERIES];
} mbQueryResults_t;

static id<MTLBuffer>				mb_queryBuffers[MB_MAX_FRAMES];	// per frame slot, MB_MAX_QUERIES counters
static uint64_t						mb_querySerial[MB_MAX_FRAMES];	// the frame whose queries a slot's buffer holds, 0 = none
static int							mb_queryCount[MB_MAX_FRAMES];
static mbQueryResults_t				mb_queryResults[MB_QUERY_HISTORY];
static int							mb_queryKeep;
static qboolean						mb_encQueries;		// the current pass has the visibility buffer
static id<MTLRenderPipelineState>	mb_queryPipeline;

static void MB_HarvestQueries( uint64_t completed );
static void MB_ProcessReadbacks( uint64_t completed );
static void *MB_Pipeline( uint32_t key );
static void MB_PrecacheKey( uint32_t key );
static void MB_PrecacheWaitAll( void );
static void MB_PrecacheFinish( qboolean report );
static void MB_SkinUniforms( void );

/*
=============================================================

GL STATE

=============================================================
*/

enum {
	MB_DIRTY_VIEWPORT		= 1 << 0,
	MB_DIRTY_SCISSOR		= 1 << 1,
	MB_DIRTY_RASTER			= 1 << 2,		// cull mode, fill mode, depth bias
	MB_DIRTY_DEPTHSTENCIL	= 1 << 3,
	MB_DIRTY_STENCILREF		= 1 << 4,
	MB_DIRTY_PIPELINE		= 1 << 5,
	MB_DIRTY_FSUNIFORMS		= 1 << 6,
	MB_DIRTY_TRANSFORM		= 1 << 7,		// mvp, eye z row, clip plane
	MB_DIRTY_ALL			= 0xff
};

static unsigned		mb_dirty = MB_DIRTY_ALL;

typedef struct {
	qboolean		enabled;
	int				size, type, stride;		// stride: actual bytes between elements
	const byte		*pointer;
	// compiled vertex array: copied once per lock if enabled when the arrays were locked
	qboolean		lockable;
	qboolean		snapValid;
	uint32_t		snapOffset;
	unsigned		snapGeneration;
} mbArray_t;

typedef struct {
	qboolean		enabled;				// MB_TEXTURE_2D
	unsigned int	texture;				// bound name
	uint32_t		env;					// MB_ENV_*
	mbArray_t		texCoords;
} mbUnit_t;

// texenv modes as the shader numbers them
#define MB_ENV_MODULATE		0
#define MB_ENV_REPLACE		1
#define MB_ENV_DECAL		2
#define MB_ENV_ADD			3
#define MB_ENV_COMBINE		4

#define MB_FS_FOG			1
#define MB_FS_RGB0			2

static qboolean		mb_blend;
static int			mb_srcBlend, mb_dstBlend;			// indices into mb_blendFactors
static qboolean		mb_depthTest;
static int			mb_depthFunc;
static qboolean		mb_depthWrite;
static double		mb_depthNear, mb_depthFar;
static float		mb_depthFloor;						// mbDepthFloor
static qboolean		mb_depthClamp;						// MB_DEPTH_CLAMP, applied with the full depth range only
static qboolean		mb_alphaTest;
static int			mb_alphaFunc;
static float		mb_alphaRef;
static qboolean		mb_cull;
static int			mb_cullFace;
static int			mb_fillMode;
static qboolean		mb_polygonOffset;
static float		mb_offsetFactor, mb_offsetUnits;
static unsigned		mb_colorMask;						// MTLColorWriteMask bits for rgb (alpha is never written)
static qboolean		mb_stencilTest;
static int			mb_stencilFunc, mb_stencilRef;
static unsigned		mb_stencilReadMask, mb_stencilWriteMask;
static int			mb_stencilFail, mb_stencilZFail, mb_stencilZPass;
static qboolean		mb_scissorTest;
static int			mb_viewport[4], mb_scissor[4];		// GL window coordinates
static qboolean		mb_scissorEmpty;
static qboolean		mb_fog;
static float		mb_fogColor[4], mb_fogStart, mb_fogEnd;
static qboolean		mb_clipPlaneOn;
static double		mb_clipPlaneEye[4];
static float		mb_clearColor[4];
static double		mb_clearDepth;
static int			mb_clearStencil;

static mbUnit_t		mb_units[MB_MAX_UNITS];
static int			mb_activeUnit, mb_clientUnit;
static mbArray_t	mb_vertexArray, mb_colorArray;
static qboolean		mb_locked;
static int			mb_lockFirst, mb_lockCount;

static int			mb_matrixMode;
static float		mb_modelview[MB_MATRIX_STACK][16];
static float		mb_projection[MB_MATRIX_STACK][16];
static int			mb_modelviewDepth, mb_projectionDepth;

// what the shader gets (tr_mtl_stage.metal MbVSUniforms / MbFSUniforms)
typedef struct {
	float		mvp[16];
	float		clipPlane[4];
	float		eyeZ[4];
	float		color[4];
	float		texCoord[4];
	uint32_t	offset[4];
	uint32_t	stride[4];
	uint32_t	posSize;
	uint32_t	colorFloat;
	uint32_t	skin;
	uint32_t	pad;
} mbVSUniforms_t;

typedef struct {
	float		fogColor[4];
	float		fogEnd;
	float		fogScale;
	float		alphaRef;
	uint32_t	alphaFunc;
	uint32_t	env0;
	uint32_t	env1;
	uint32_t	flags;
	uint32_t	pad;
} mbFSUniforms_t;

static mbVSUniforms_t	mb_vsu;

// what mb_vsu's transform was computed from (MB_UpdateTransform)
static float		mb_vsuProjection[16], mb_vsuModelview[16];
static double		mb_vsuClipPlaneEye[4];
static qboolean		mb_vsuClipPlaneOn, mb_vsuTransformValid;

// what the current encoder has
static uint32_t		mb_encPipelineKey;
static void			*mb_encDepthStencil;
static qboolean		mb_encDepthClamp;					// MTLDepthClipModeClamp (a new encoder clips)
static mbFSUniforms_t	mb_encFSU;
static qboolean		mb_encFSUValid;
static void			*mb_encTexture[MB_MAX_UNITS], *mb_encSampler[MB_MAX_UNITS];
static __unsafe_unretained id<MTLBuffer>	mb_encRing;
static uint32_t		mb_texMask;							// units sampled by the current draw
static uint32_t		mb_texFlags;						// MB_FS_RGB* of the current draw

// GPU skinning (mbSkin*, tr_mtl_stage.metal mb_skin_vs)
#define MB_SKIN_BUFFER_SIZE	( 4 * 1024 * 1024 )			// to start with; doubled when full
#define MB_SKIN_BUFFER_MAX	( 256 * 1024 * 1024 )
#define MB_SKIN_COPIES		64

typedef struct {
	uint32_t	firstVertex, numVertexes;
	uint32_t	surface, boneMap;
	uint32_t	bones, morphs;
	float		scale;
	uint32_t	pad;
} mbSkinJobUniforms_t;									// MbSkinJob

typedef struct {
	const void	*data;
	int			bytes;
	uint32_t	offset;
} mbSkinCopy_t;

static id<MTLFunction>		mb_skinVS;
static id<MTLBuffer>		mb_skinBuffer;				// the static data; replaced by a bigger copy when full
static NSUInteger			mb_skinUsed;
static __unsafe_unretained id<MTLBuffer>	mb_encSkin;		// what the encoder has at vertex buffer 2
static const mbSkinDraw_t	*mb_skinDraw;				// between mbSkinBegin and mbSkinEnd
static mbSkinCopy_t			mb_skinCopies[MB_SKIN_COPIES];	// the scene's bone and morph copies in the ring
static int					mb_numSkinCopies;
static uint64_t				mb_skinCopySerial;
static unsigned				mb_skinCopyGeneration, mb_skinCopyScene;

// static vertex data (mbStatic*: the world's faces)
#define MB_STATIC_BUFFER_SIZE	( 1024 * 1024 )			// to start with; doubled when full
#define MB_STATIC_BUFFER_MAX	( 256 * 1024 * 1024 )
static id<MTLBuffer>		mb_staticBuffer;			// replaced by a bigger copy when full
static NSUInteger			mb_staticUsed;
static qboolean				mb_staticArrays;			// array pointers are offsets into mb_staticBuffer

/*
=============================================================

CACHES

=============================================================
*/

typedef struct {
	uint32_t	key;		// key + 1; 0 = empty
	void		*object;	// retained
} mbCacheSlot32_t;

typedef struct {
	uint64_t	key;		// key + 1; 0 = empty
	void		*object;	// retained
} mbCacheSlot64_t;

static mbCacheSlot32_t	mb_pipelines[MB_PIPELINE_SLOTS];
static mbCacheSlot64_t	mb_depthStencils[MB_DS_SLOTS];
static mbCacheSlot32_t	mb_samplers[MB_SAMPLER_SLOTS];

static const MTLBlendFactor mb_blendFactors[] = {
	MTLBlendFactorZero,
	MTLBlendFactorOne,
	MTLBlendFactorSourceColor,
	MTLBlendFactorOneMinusSourceColor,
	MTLBlendFactorSourceAlpha,
	MTLBlendFactorOneMinusSourceAlpha,
	MTLBlendFactorDestinationAlpha,
	MTLBlendFactorOneMinusDestinationAlpha,
	MTLBlendFactorDestinationColor,
	MTLBlendFactorOneMinusDestinationColor,
	MTLBlendFactorSourceAlphaSaturated,
};

/*
=============================================================

TEXTURES

=============================================================
*/

typedef struct {
	void			*texture;		// id<MTLTexture>, retained; NULL until the first image
	void			*sampler;		// id<MTLSamplerState>, owned by the sampler cache
	uint64_t		usedSerial;		// newest frame that sampled it
	uint64_t		blitSerial;		// newest frame whose upload command buffer writes it
	int				width, height;	// level 0
	int				levels;			// allocated mip levels
	int				definedLevels;	// levels with an image
	MTLPixelFormat	format;
	qboolean		rgbOnly;		// MB_RGB-like internal format: samples alpha 1, replace keeps the incoming alpha
	int				minFilter, magFilter, wrapS, wrapT, anisotropy;
	qboolean		samplerDirty;
	qboolean		allocated;		// the name is in use
	int				nextFree;		// name queue
} mbTexture_t;

static mbTexture_t	*mb_textures;
static int			mb_maxTextures;
static unsigned int	mb_nextTextureName = 1;			// 0 is "no texture", as in GL
static int			mb_freeHead = -1, mb_freeTail = -1, mb_numFree;

typedef struct {
	void		*texture;		// retained
	uint64_t	serial;			// usable once completed
} mbPooled_t;

static mbPooled_t	mb_texturePool[MB_TEXTURE_POOL];
static int			mb_numPooled;

static byte			*mb_scratch;					// RGB -> RGBA conversion, mip copies
static size_t		mb_scratchSize;

/*
=============================================================

HELPERS

=============================================================
*/

static void MB_Retire( id object )
{
	if ( !object ) {
		return;
	}
	if ( mb_numRetired == mb_maxRetired ) {
		mb_maxRetired = mb_maxRetired ? mb_maxRetired * 2 : 256;
		mb_retired = realloc( mb_retired, mb_maxRetired * sizeof( *mb_retired ) );
	}
	mb_retired[mb_numRetired].object = (__bridge_retained void *)object;
	mb_retired[mb_numRetired].serial = mb_frameSerial;
	mb_numRetired++;
}

static void MB_ReleaseRetired( uint64_t completed )
{
	int i, kept;

	for ( i = kept = 0; i < mb_numRetired; i++ ) {
		if ( mb_retired[i].serial <= completed ) {
			MB_RELEASE( mb_retired[i].object );
		} else {
			mb_retired[kept++] = mb_retired[i];
		}
	}
	mb_numRetired = kept;
}

static byte *MB_Scratch( size_t size )
{
	if ( size > mb_scratchSize ) {
		mb_scratchSize = size + ( size >> 1 );
		mb_scratch = realloc( mb_scratch, mb_scratchSize );
	}
	return mb_scratch;
}

static void MB_Identity( float *m )
{
	memset( m, 0, 16 * sizeof( float ) );
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b (column-major, GL order); out may not alias a or b
static void MB_MultiplyMatrix( const float *a, const float *b, float *out )
{
	int c, r;

	for ( c = 0; c < 4; c++ ) {
		for ( r = 0; r < 4; r++ ) {
			out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1]
				+ a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
		}
	}
}

// general 4x4 inverse (column-major); returns qfalse if singular
static qboolean MB_InvertMatrix( const float *m, double *inv )
{
	double det;
	int i;

	inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
	inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
	inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
	inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
	inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
	inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if ( det == 0.0 ) {
		return qfalse;
	}
	det = 1.0 / det;
	for ( i = 0; i < 16; i++ ) {
		inv[i] *= det;
	}
	return qtrue;
}

static int MB_BlendIndex( int factor )
{
	switch ( factor ) {
	case MB_ZERO:					return 0;
	case MB_ONE:					return 1;
	case MB_SRC_COLOR:				return 2;
	case MB_ONE_MINUS_SRC_COLOR:	return 3;
	case MB_SRC_ALPHA:				return 4;
	case MB_ONE_MINUS_SRC_ALPHA:	return 5;
	case MB_DST_ALPHA:				return 6;
	case MB_ONE_MINUS_DST_ALPHA:	return 7;
	case MB_DST_COLOR:				return 8;
	case MB_ONE_MINUS_DST_COLOR:	return 9;
	case MB_SRC_ALPHA_SATURATE:		return 10;
	default:						return 1;
	}
}

static int MB_CompareIndex( int func )
{
	// MTLCompareFunction has GL's order (never, less, equal, lequal, greater, notequal, gequal, always)
	return ( func >= MB_NEVER && func <= MB_ALWAYS ) ? func - MB_NEVER : MTLCompareFunctionAlways;
}

static int MB_StencilOpIndex( int op )
{
	switch ( op ) {
	case MB_ZERO:		return MTLStencilOperationZero;
	case MB_REPLACE:	return MTLStencilOperationReplace;
	case MB_INCR:		return MTLStencilOperationIncrementClamp;
	case MB_DECR:		return MTLStencilOperationDecrementClamp;
	default:			return MTLStencilOperationKeep;
	}
}

/*
=============================================================

BUFFER RING AND COMMAND BUFFERS

=============================================================
*/

static void MB_SelectRing( int slot )
{
	mb_frameSlot = slot;
	mb_ring = mb_ringBuffers[slot];
	mb_ringBase = mb_ring.contents;
	mb_ringSize = mb_ring.length;
	mb_ringUsed = 0;
	mb_ringGeneration++;
}

static id<MTLBuffer> MB_NewRingBuffer( NSUInteger size )
{
	id<MTLBuffer> buffer = [mb_device newBufferWithLength:size
		options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];

	buffer.label = @"frame ring";
	return buffer;
}

/*
** MB_RingAlloc
**
** Space in this frame's ring. If the slot is full, it gets a bigger buffer (the old one is retired), which
** bumps mb_ringGeneration: offsets taken before belong to the old buffer.
*/
static byte *MB_RingAlloc( NSUInteger size, uint32_t *offset )
{
	NSUInteger start = ( mb_ringUsed + MB_RING_ALIGN - 1 ) & ~(NSUInteger)( MB_RING_ALIGN - 1 );

	if ( start + size > mb_ringSize ) {
		NSUInteger newSize = mb_ringSize * 2;

		while ( newSize < size * 2 ) {
			newSize *= 2;
		}
		MB_Retire( mb_ring );
		mb_ringBuffers[mb_frameSlot] = MB_NewRingBuffer( newSize );
		mb_ring = mb_ringBuffers[mb_frameSlot];
		mb_ringBase = mb_ring.contents;
		mb_ringSize = newSize;
		mb_ringGeneration++;
		mb_statRingGrowths++;
		start = 0;
	}

	mb_ringUsed = start + size;
	mb_statRingBytes += size;
	*offset = (uint32_t)start;
	return mb_ringBase + start;
}

static void MB_BeginCommandBuffer( void )
{
	if ( !mb_cmd ) {
		@autoreleasepool {
			mb_cmd = [mb_queue commandBufferWithUnretainedReferences];
		}
	}
}

static id<MTLBlitCommandEncoder> MB_UploadBlit( void )
{
	if ( !mb_uploadBlit ) {
		@autoreleasepool {
			mb_uploadCmd = [mb_queue commandBufferWithUnretainedReferences];
			mb_uploadCmd.label = @"texture uploads";
			mb_uploadBlit = [mb_uploadCmd blitCommandEncoder];
		}
	}
	return mb_uploadBlit;
}

// the upload command buffer runs before everything the frame encodes (it is committed first)
static void MB_CommitUploads( void )
{
	MB_CancelLatePresent();
	if ( mb_uploadBlit ) {
		[mb_uploadBlit endEncoding];
		[mb_uploadCmd commit];
		mb_uploadBlit = nil;
		mb_uploadCmd = nil;
	}
}

static void MB_InvalidateEncoderState( void )
{
	int i;

	mb_encPipelineKey = 0xffffffff;
	mb_encDepthStencil = NULL;
	mb_encDepthClamp = qfalse;
	mb_encFSUValid = qfalse;
	mb_encRing = nil;
	mb_encSkin = nil;
	for ( i = 0; i < MB_MAX_UNITS; i++ ) {
		mb_encTexture[i] = NULL;
		mb_encSampler[i] = NULL;
	}
	mb_dirty = MB_DIRTY_ALL;
}

/*
** MB_BeginPass
**
** The scene pass, begun at the first draw or partial clear of the frame (or after an interruption). Full
** clears requested before it are folded into its load actions.
*/
static qboolean MB_BeginPass( void )
{
	MTLRenderPassDescriptor *pass;

	if ( mb_enc ) {
		return qtrue;
	}
	if ( !mb_sceneColor ) {
		return qfalse;
	}

	MB_BeginCommandBuffer();

	@autoreleasepool {
		pass = [MTLRenderPassDescriptor renderPassDescriptor];

		pass.colorAttachments[0].texture = mb_sceneColor;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		if ( mb_pendingClear & MB_COLOR_BUFFER_BIT ) {
			// alpha stays 1: the target behaves like GL1's 24-bit color buffer (destination alpha = 1)
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].clearColor = MTLClearColorMake( mb_pendingClearColor[0], mb_pendingClearColor[1], mb_pendingClearColor[2], 1.0 );
		} else if ( !mb_sceneColorValid ) {
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].clearColor = MTLClearColorMake( 0.0, 0.0, 0.0, 1.0 );
		} else {
			pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
		}

		// depth/stencil: a new frame starts cleared (the front end always clears depth before 3D); stored only
		// when the pass is interrupted (MTLStoreActionUnknown: decided in MB_EndPass)
		pass.depthAttachment.texture = mb_sceneDepth;
		pass.depthAttachment.storeAction = MTLStoreActionUnknown;
		if ( ( mb_pendingClear & MB_DEPTH_BUFFER_BIT ) || !mb_depthValid ) {
			pass.depthAttachment.loadAction = MTLLoadActionClear;
			pass.depthAttachment.clearDepth = ( mb_pendingClear & MB_DEPTH_BUFFER_BIT ) ? mb_pendingClearDepth : 1.0;
		} else {
			pass.depthAttachment.loadAction = MTLLoadActionLoad;
		}

		pass.stencilAttachment.texture = mb_sceneDepth;
		pass.stencilAttachment.storeAction = MTLStoreActionUnknown;
		if ( ( mb_pendingClear & MB_STENCIL_BUFFER_BIT ) || !mb_depthValid ) {
			pass.stencilAttachment.loadAction = MTLLoadActionClear;
			pass.stencilAttachment.clearStencil = ( mb_pendingClear & MB_STENCIL_BUFFER_BIT ) ? ( mb_pendingClearStencil & 0xff ) : 0;
		} else {
			pass.stencilAttachment.loadAction = MTLLoadActionLoad;
		}

		// depth queries count into the frame slot's buffer while they are in use (accumulated across the
		// frame's passes: every query has its own counter, zeroed when its results are copied out)
		mb_encQueries = ( mb_queryKeep > 0 && mb_queryBuffers[mb_frameSlot] ) ? qtrue : qfalse;
		if ( mb_encQueries ) {
			pass.visibilityResultBuffer = mb_queryBuffers[mb_frameSlot];
			pass.visibilityResultType = MTLVisibilityResultTypeAccumulate;
		}

		mb_enc = [mb_cmd renderCommandEncoderWithDescriptor:pass];
	}

	mb_pendingClear = 0;
	mb_sceneColorValid = qtrue;
	mb_statPasses++;

	[mb_enc setFrontFacingWinding:MTLWindingCounterClockwise];
	MB_InvalidateEncoderState();
	return qtrue;
}

/*
** MB_EndPass
**
** keepDepth: the frame continues in another pass (readback), so depth/stencil are stored.
*/
static void MB_EndPass( qboolean keepDepth )
{
	if ( !mb_enc ) {
		return;
	}
	[mb_enc setDepthStoreAction:keepDepth ? MTLStoreActionStore : MTLStoreActionDontCare];
	[mb_enc setStencilStoreAction:keepDepth ? MTLStoreActionStore : MTLStoreActionDontCare];
	[mb_enc endEncoding];
	mb_enc = nil;
	mb_depthValid = keepDepth;
}

/*
** MB_FlushAndWait
**
** Submits everything recorded so far in this frame and waits for it (readback only).
*/
static void MB_FlushAndWait( void )
{
	if ( mb_pendingClear ) {
		MB_BeginPass();		// apply the clears before reading
	}
	MB_EndPass( qtrue );
	MB_CommitUploads();
	if ( mb_cmd ) {
		[mb_cmd commit];
		[mb_cmd waitUntilCompleted];
		mb_cmd = nil;
	}
}

/*
=============================================================

PIPELINE CACHE

=============================================================
*/

// Compiled pipelines are kept in an MTLBinaryArchive in ~/Library/Caches/<bundle ID>/metal/ (<fs_homepath>/metal/ when
// fs_homepath is given, as the port's tools do), next to the list of the pipeline keys used. The first mbInit of a run
// creates the listed pipelines from the archive: nothing is compiled, and no pipeline is first created in the middle of
// a match (with a warm system shader cache, the first pipeline of a run took 42.6 ms to create). A pipeline the archive
// lacks is compiled as before. When a run compiled any, both files are written at mbShutdown (vid_restart, quit), never
// during play, the archive as a new one holding every pipeline the process created: an archive read from a file, with
// pipelines added, fails to serialize for some sets of pipelines ("expecting 'fragment' stage in pipeline no. N",
// macOS 26), and after a failure it writes only what was added after it (session 27). The file names carry a hash of
// the embedded metallib, the GPU, the OS version and the cache version, so a new build, GPU or OS starts a new cache;
// the other files are deleted.
#define MB_CACHE_MAGIC		0x4350424d		// "MBPC"
#define MB_CACHE_VERSION	2				// 1 added to the archive it read, which lost most of its pipelines

static id<MTLBinaryArchive>		mb_archive;			// the archive read at startup (lookups only)
static NSMutableArray			*mb_archiveDescs;	// descriptors of the pipelines created, for the next archive written
static qboolean					mb_archiveDirty;	// a pipeline was compiled since the archive was read or written
static NSString					*mb_archivePath, *mb_keysPath;
static uint32_t					mb_cacheKeys[MB_PIPELINE_SLOTS];	// stage/clear pipeline keys created in this process
static int						mb_numCacheKeys, mb_savedCacheKeys;
static qboolean					mb_cacheOpened;

// Pipelines compiling on Metal's threads (MB_PrecacheKey): the completion handler writes a slot's pipeline, error and
// end time, then signals its semaphore; the main thread reads them after waiting on it (MB_PrecacheComplete).
#define MB_PRECACHE_SLOTS	64

static uint32_t						mb_precacheKeys[MB_PRECACHE_SLOTS];
static MTLRenderPipelineDescriptor	*mb_precacheDescs[MB_PRECACHE_SLOTS];
static dispatch_semaphore_t			mb_precacheDone[MB_PRECACHE_SLOTS];	// nil: the slot is complete
static id<MTLRenderPipelineState>	mb_precacheResults[MB_PRECACHE_SLOTS];
static NSError						*mb_precacheErrors[MB_PRECACHE_SLOTS];
static uint64_t						mb_precacheSubmit[MB_PRECACHE_SLOTS], mb_precacheEnd[MB_PRECACHE_SLOTS];	// mach time
static int							mb_numPrecache;
static int							mb_precacheCompiled;	// since the last MB_PrecacheFinish
static uint64_t						mb_precacheSlowest;		// mach ticks from submission to the end, since then

/*
** MB_ArchivePipeline
**
** The pipeline from the archive, nil if it doesn't hold it. Either way the descriptor is kept for the next archive.
*/
static id<MTLRenderPipelineState> MB_ArchivePipeline( MTLRenderPipelineDescriptor *desc )
{
	id<MTLRenderPipelineState> pipeline;

	if ( !mb_archive ) {
		return nil;
	}
	desc.binaryArchives = @[ mb_archive ];
	pipeline = [mb_device newRenderPipelineStateWithDescriptor:desc options:MTLPipelineOptionFailOnBinaryArchiveMiss
		reflection:nil error:nil];
	desc.binaryArchives = nil;
	[mb_archiveDescs addObject:desc];
	if ( pipeline ) {
		mb_statPipelinesCached++;
	} else {
		mb_archiveDirty = qtrue;
	}
	return pipeline;
}

/*
** MB_NewRenderPipeline
**
** From the archive if it holds the pipeline, else compiled.
*/
static id<MTLRenderPipelineState> MB_NewRenderPipeline( MTLRenderPipelineDescriptor *desc, NSError **error )
{
	id<MTLRenderPipelineState> pipeline = MB_ArchivePipeline( desc );

	if ( !pipeline ) {
		pipeline = [mb_device newRenderPipelineStateWithDescriptor:desc error:error];
		if ( !pipeline ) {
			[mb_archiveDescs removeObjectIdenticalTo:desc];
		}
	}
	return pipeline;
}

/*
** MB_CacheHash
**
** FNV-1a over the metallib, the GPU's name, the OS version and the cache version.
*/
static uint32_t MB_CacheHash( void )
{
	const char *strings[2];
	uint32_t hash = 2166136261u;
	const char *c;
	unsigned int i;

	for ( i = 0; i < tr_mtl_metallib_len; i++ ) {
		hash = ( hash ^ tr_mtl_metallib[i] ) * 16777619u;
	}
	strings[0] = mb_device.name.UTF8String;
	strings[1] = [NSProcessInfo processInfo].operatingSystemVersionString.UTF8String;
	for ( i = 0; i < 2; i++ ) {
		for ( c = strings[i]; c && *c; c++ ) {
			hash = ( hash ^ (byte)*c ) * 16777619u;
		}
	}
	return ( hash ^ MB_CACHE_VERSION ) * 16777619u;
}

/*
** MB_OpenPipelineCache
**
** Once per process, before the first pipeline is created.
*/
static void MB_OpenPipelineCache( void )
{
	NSFileManager *fm = [NSFileManager defaultManager];
	MTLBinaryArchiveDescriptor *desc;
	NSString *dir, *prefix, *name, *bundleId;
	NSError *error = nil;
	cvar_t *home;
	const char *userHome;

	if ( mb_cacheOpened ) {
		return;
	}
	mb_cacheOpened = qtrue;

	if ( !r_metalPipelineCache->integer ) {
		return;
	}

	@autoreleasepool {
		// fs_homepath is empty unless given at launch: a player's run keeps the cache in ~/Library/Caches (regenerated
		// if macOS clears it, never synced, not in the game folder), from $HOME as the engine's homes are (sys_unix.c)
		home = ri.Cvar_Get( "fs_homepath", "", 0 );
		if ( home->string[0] ) {
			dir = [NSString stringWithUTF8String:home->string];
		} else {
			userHome = getenv( "HOME" );
			bundleId = [NSBundle mainBundle].bundleIdentifier;
			if ( !userHome || !userHome[0] || !bundleId ) {
				return;
			}
			dir = [[NSString stringWithUTF8String:userHome] stringByAppendingFormat:@"/Library/Caches/%@", bundleId];
		}
		dir = [dir stringByAppendingPathComponent:@"metal"];
		if ( ![fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&error] ) {
			ri.Printf( PRINT_WARNING, "Metal: no pipeline cache: %s\n", error.localizedDescription.UTF8String );
			return;
		}
		prefix = [NSString stringWithFormat:@"pipelines-%08x.", MB_CacheHash()];
		mb_archivePath = [dir stringByAppendingPathComponent:[prefix stringByAppendingString:@"bin"]];
		mb_keysPath = [dir stringByAppendingPathComponent:[prefix stringByAppendingString:@"keys"]];

		// the caches of other builds, GPUs or OS versions
		for ( name in [fm contentsOfDirectoryAtPath:dir error:nil] ) {
			if ( [name hasPrefix:@"pipelines-"] && ![name hasPrefix:prefix] ) {
				[fm removeItemAtPath:[dir stringByAppendingPathComponent:name] error:nil];
			}
		}

		desc = [MTLBinaryArchiveDescriptor new];
		if ( [fm fileExistsAtPath:mb_archivePath] ) {
			desc.url = [NSURL fileURLWithPath:mb_archivePath];
		}
		mb_archive = [mb_device newBinaryArchiveWithDescriptor:desc error:&error];
		if ( !mb_archive && desc.url ) {
			ri.Printf( PRINT_WARNING, "Metal: pipeline cache unreadable, starting a new one: %s\n", error.localizedDescription.UTF8String );
			[fm removeItemAtPath:mb_keysPath error:nil];
			desc.url = nil;
			mb_archive = [mb_device newBinaryArchiveWithDescriptor:desc error:&error];
		}
		if ( !mb_archive ) {
			ri.Printf( PRINT_WARNING, "Metal: no pipeline cache: %s\n", error.localizedDescription.UTF8String );
			return;
		}
		mb_archive.label = @"pipeline cache";
		mb_archiveDescs = [NSMutableArray new];
	}
}

/*
** MB_PrewarmPipelines
**
** Creates the pipelines the cache lists (once per process, after the shader library is loaded): those in the archive
** at once, the others compiled in parallel on Metal's threads.
*/
static void MB_PrewarmPipelines( void )
{
	static qboolean done;
	uint32_t header[3], key;
	uint64_t t0;
	FILE *f;
	int i, count, cached;

	if ( done || !mb_archive ) {
		return;
	}
	done = qtrue;

	f = fopen( mb_keysPath.fileSystemRepresentation, "rb" );
	if ( !f ) {
		return;
	}
	t0 = mach_absolute_time();
	count = mb_numCacheKeys;
	cached = mb_statPipelinesCached;
	if ( fread( header, sizeof( header ), 1, f ) == 1 && header[0] == MB_CACHE_MAGIC && header[1] == MB_CACHE_VERSION ) {
		for ( i = 0; i < (int)header[2] && fread( &key, sizeof( key ), 1, f ) == 1; i++ ) {
			MB_PrecacheKey( key );
		}
	}
	fclose( f );
	MB_PrecacheFinish( qfalse );
	mb_savedCacheKeys = mb_numCacheKeys;

	{
		mach_timebase_info_data_t tb;

		mach_timebase_info( &tb );
		ri.Printf( PRINT_ALL, "Metal: %d pipelines created up front (%d from the pipeline cache) in %.1f ms\n",
			mb_numCacheKeys - count, mb_statPipelinesCached - cached,
			(double)( mach_absolute_time() - t0 ) * tb.numer / tb.denom / 1e6 );
	}
}

/*
** MB_SavePipelineCache
**
** When a pipeline was compiled since the archive was read or written, writes a new archive with every pipeline created
** (to a temporary file, then renamed over the old one, which the archive read at startup may still have mapped; a
** failure leaves the old one); and the keys file when the list grew. A key the archive lacks is compiled up front by
** the next run, never in play, so the keys are written even when the archive isn't.
*/
static void MB_SavePipelineCache( void )
{
	id<MTLBinaryArchive> archive;
	MTLRenderPipelineDescriptor *desc;
	NSString *tmp;
	NSError *error = nil;
	uint32_t header[3];
	FILE *f;

	if ( !mb_archive || ( !mb_archiveDirty && mb_numCacheKeys == mb_savedCacheKeys ) ) {
		return;
	}

	@autoreleasepool {
		if ( mb_archiveDirty ) {
			archive = [mb_device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error];
			for ( desc in mb_archiveDescs ) {
				if ( archive && ![archive addRenderPipelineFunctionsWithDescriptor:desc error:&error] ) {
					ri.Printf( PRINT_WARNING, "Metal: pipeline cache: %s\n", error.localizedDescription.UTF8String );
				}
			}

			tmp = [mb_archivePath stringByAppendingString:@".tmp"];
			error = nil;
			if ( archive && [archive serializeToURL:[NSURL fileURLWithPath:tmp] error:&error]
				&& !rename( tmp.fileSystemRepresentation, mb_archivePath.fileSystemRepresentation ) ) {
				mb_archiveDirty = qfalse;
			} else {
				ri.Printf( PRINT_ALL, "Metal: pipeline cache not written: %s\n", error ? error.localizedDescription.UTF8String : strerror( errno ) );
				unlink( tmp.fileSystemRepresentation );
			}
		}

		if ( mb_numCacheKeys != mb_savedCacheKeys ) {
			tmp = [mb_keysPath stringByAppendingString:@".tmp"];
			f = fopen( tmp.fileSystemRepresentation, "wb" );
			if ( !f ) {
				return;
			}
			header[0] = MB_CACHE_MAGIC;
			header[1] = MB_CACHE_VERSION;
			header[2] = mb_numCacheKeys;
			fwrite( header, sizeof( header ), 1, f );
			fwrite( mb_cacheKeys, sizeof( mb_cacheKeys[0] ), mb_numCacheKeys, f );
			if ( fclose( f ) == 0 ) {
				rename( tmp.fileSystemRepresentation, mb_keysPath.fileSystemRepresentation );
			}
		}
	}
	mb_savedCacheKeys = mb_numCacheKeys;
}

/*
=============================================================

PIPELINES, DEPTH/STENCIL STATES, SAMPLERS

=============================================================
*/

static id<MTLFunction> MB_StageFragmentFunction( uint32_t texMask, qboolean alphaTest )
{
	int index = texMask | ( alphaTest ? 4 : 0 );

	if ( !mb_stageFS[index] ) {
		MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
		uint32_t mask = texMask;
		bool discard = alphaTest ? true : false;
		NSError *error = nil;

		[values setConstantValue:&mask type:MTLDataTypeUInt atIndex:0];
		[values setConstantValue:&discard type:MTLDataTypeBool atIndex:1];
		mb_stageFS[index] = [mb_library newFunctionWithName:@"mb_stage_fs" constantValues:values error:&error];
		if ( !mb_stageFS[index] ) {
			ri.Printf( PRINT_WARNING, "Metal: stage shader variant %d failed: %s\n", index, error.localizedDescription.UTF8String );
		}
	}
	return mb_stageFS[index];
}

// key bits: 0-1 texture units, 2 alpha test, 3 blend, 4-7 src factor, 8-11 dst factor, 12-15 color write
// mask (MTLColorWriteMask >> 0, alpha never set), 16 clear pipeline, 17 skinning vertex shader
#define MB_PIPE_CLEAR		( 1u << 16 )
#define MB_PIPE_SKIN		( 1u << 17 )

static uint32_t MB_PipelineKey( void )
{
	uint32_t key = mb_texMask;

	if ( mb_alphaTest && mb_alphaFunc != MB_ALWAYS ) {
		key |= 1u << 2;
	}
	if ( mb_blend ) {
		key |= ( 1u << 3 ) | ( (uint32_t)mb_srcBlend << 4 ) | ( (uint32_t)mb_dstBlend << 8 );
	}
	key |= mb_colorMask << 12;
	if ( mb_skinDraw ) {
		key |= MB_PIPE_SKIN;
	}
	return key;
}

/*
** MB_PipelineDescriptor
**
** The descriptor of a stage/clear pipeline key, nil if its fragment function failed.
*/
static MTLRenderPipelineDescriptor *MB_PipelineDescriptor( uint32_t key )
{
	MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
	MTLRenderPipelineColorAttachmentDescriptor *color = desc.colorAttachments[0];

	if ( key & MB_PIPE_CLEAR ) {
		desc.label = @"clear";
		desc.vertexFunction = mb_clearVS;
		desc.fragmentFunction = mb_clearFS;
	} else {
		desc.label = ( key & MB_PIPE_SKIN ) ? @"skin" : @"stage";
		desc.vertexFunction = ( key & MB_PIPE_SKIN ) ? mb_skinVS : mb_stageVS;
		desc.fragmentFunction = MB_StageFragmentFunction( key & 3, ( key >> 2 ) & 1 );
		if ( !desc.fragmentFunction ) {
			return nil;
		}
	}

	color.pixelFormat = MTLPixelFormatRGBA8Unorm;
	color.writeMask = ( key >> 12 ) & 15;
	if ( !( key & MB_PIPE_CLEAR ) && ( key & ( 1u << 3 ) ) ) {
		color.blendingEnabled = YES;
		color.sourceRGBBlendFactor = color.sourceAlphaBlendFactor = mb_blendFactors[( key >> 4 ) & 15];
		color.destinationRGBBlendFactor = color.destinationAlphaBlendFactor = mb_blendFactors[( key >> 8 ) & 15];
		color.rgbBlendOperation = color.alphaBlendOperation = MTLBlendOperationAdd;
	}
	desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
	desc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
	return desc;
}

// main-thread time spent creating pipelines (r_metalPresentStats)
static void MB_PipelineTime( uint64_t ticks )
{
	mb_statPipelineTime += ticks;
	if ( ticks > mb_statPipelineMax ) {
		mb_statPipelineMax = ticks;
	}
}

static double MB_TicksMs( uint64_t ticks )
{
	static mach_timebase_info_data_t tb;

	if ( !tb.denom ) {
		mach_timebase_info( &tb );
	}
	return (double)ticks * tb.numer / tb.denom / 1e6;
}

// the key's slot in mb_pipelines, or the empty slot it goes into
static uint32_t MB_PipelineSlot( uint32_t key )
{
	uint32_t i = ( key * 2654435761u ) & ( MB_PIPELINE_SLOTS - 1 );

	while ( mb_pipelines[i].key && mb_pipelines[i].key != key + 1 ) {
		i = ( i + 1 ) & ( MB_PIPELINE_SLOTS - 1 );
	}
	return i;
}

static void MB_SetPipeline( uint32_t key, id<MTLRenderPipelineState> pipeline )
{
	uint32_t i = MB_PipelineSlot( key );

	mb_pipelines[i].object = (__bridge_retained void *)pipeline;
	mb_pipelines[i].key = key + 1;
	mb_statPipelines++;
	if ( mb_numCacheKeys < MB_PIPELINE_SLOTS ) {
		mb_cacheKeys[mb_numCacheKeys++] = key;		// for the pipeline cache
	}
}

/*
** MB_PrecacheKey
**
** Creates a key's pipeline ahead of its first draw: from the archive at once, else compiled on Metal's threads.
*/
static void MB_PrecacheKey( uint32_t key )
{
	MTLRenderPipelineDescriptor *desc;
	id<MTLRenderPipelineState> pipeline;
	dispatch_semaphore_t done;
	uint64_t t0;
	int i, n;

	if ( mb_pipelines[MB_PipelineSlot( key )].key ) {
		return;
	}
	for ( i = 0; i < mb_numPrecache; i++ ) {
		if ( mb_precacheKeys[i] == key && mb_precacheDone[i] ) {
			return;
		}
	}
	if ( mb_numPrecache == MB_PRECACHE_SLOTS ) {
		MB_PrecacheWaitAll();
	}

	@autoreleasepool {
		t0 = mach_absolute_time();
		desc = MB_PipelineDescriptor( key );
		if ( !desc ) {
			return;
		}
		pipeline = MB_ArchivePipeline( desc );
		if ( pipeline ) {
			MB_PipelineTime( mach_absolute_time() - t0 );
			MB_SetPipeline( key, pipeline );
			return;
		}

		n = mb_numPrecache++;
		mb_precacheSubmit[n] = t0;
		mb_precacheKeys[n] = key;
		mb_precacheDescs[n] = desc;
		done = mb_precacheDone[n] = dispatch_semaphore_create( 0 );
		[mb_device newRenderPipelineStateWithDescriptor:desc completionHandler:^( id<MTLRenderPipelineState> result, NSError *error ) {
			mb_precacheResults[n] = result;
			mb_precacheErrors[n] = error;
			mb_precacheEnd[n] = mach_absolute_time();
			dispatch_semaphore_signal( done );
		}];
		MB_PipelineTime( mach_absolute_time() - t0 );	// what the main thread spent on it
	}
}

/*
** MB_PrecacheComplete
**
** Waits for a slot's compile and installs its pipeline.
*/
static void MB_PrecacheComplete( int n )
{
	dispatch_semaphore_wait( mb_precacheDone[n], DISPATCH_TIME_FOREVER );
	if ( mb_precacheResults[n] ) {
		MB_SetPipeline( mb_precacheKeys[n], mb_precacheResults[n] );
		mb_statPipelinesAhead++;
		mb_precacheCompiled++;
		if ( mb_precacheEnd[n] - mb_precacheSubmit[n] > mb_precacheSlowest ) {
			mb_precacheSlowest = mb_precacheEnd[n] - mb_precacheSubmit[n];
		}
	} else {
		ri.Printf( PRINT_WARNING, "Metal: pipeline 0x%x failed: %s\n", mb_precacheKeys[n],
			mb_precacheErrors[n].localizedDescription.UTF8String );
		[mb_archiveDescs removeObjectIdenticalTo:mb_precacheDescs[n]];
	}
	mb_precacheDone[n] = nil;
	mb_precacheDescs[n] = nil;
	mb_precacheResults[n] = nil;
	mb_precacheErrors[n] = nil;
}

static void MB_PrecacheWaitAll( void )
{
	int i;

	for ( i = 0; i < mb_numPrecache; i++ ) {
		if ( mb_precacheDone[i] ) {
			MB_PrecacheComplete( i );
		}
	}
	mb_numPrecache = 0;
}

/*
** MB_PrecacheFinish
**
** Waits for every pipeline still compiling; report: prints what was compiled since the last call.
*/
static void MB_PrecacheFinish( qboolean report )
{
	uint64_t t0 = mach_absolute_time();

	MB_PrecacheWaitAll();
	if ( report && mb_precacheCompiled ) {
		ri.Printf( PRINT_ALL, "Metal: %d pipelines compiled ahead of their first draw (the slowest in %.1f ms; %.1f ms waited for at the end)\n",
			mb_precacheCompiled, MB_TicksMs( mb_precacheSlowest ), MB_TicksMs( mach_absolute_time() - t0 ) );
	}
	mb_precacheCompiled = 0;
	mb_precacheSlowest = 0;
}

/*
** mbPrecachePipeline
**
** The pipeline MB_PipelineKey gives for that state with the color mask on, for units 0..numUnits-1 bound (none: 0,
** an untextured draw).
*/
void mbPrecachePipeline( int numUnits, qboolean alphaTest, qboolean blend, int sfactor, int dfactor, qboolean skin )
{
	uint32_t key = numUnits <= 0 ? 0 : ( numUnits > 1 && mbMultitexture ) ? 3 : 1;

	if ( !mb_library || ( skin && !mb_skinVS ) ) {
		return;
	}
	if ( alphaTest ) {
		key |= 1u << 2;
	}
	if ( blend ) {
		key |= ( 1u << 3 ) | ( (uint32_t)MB_BlendIndex( sfactor ) << 4 ) | ( (uint32_t)MB_BlendIndex( dfactor ) << 8 );
	}
	key |= (uint32_t)( MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue ) << 12;
	if ( skin ) {
		key |= MB_PIPE_SKIN;
	}
	MB_PrecacheKey( key );
}

/*
** mbPrecacheFinish
*/
void mbPrecacheFinish( void )
{
	MB_PrecacheFinish( qtrue );
}

/*
** MB_Pipeline
**
** The pipeline of a key: created at its first draw if nothing created it ahead (waited for if it is compiling).
*/
static void *MB_Pipeline( uint32_t key )
{
	uint32_t i = MB_PipelineSlot( key );
	id<MTLRenderPipelineState> pipeline;
	MTLRenderPipelineDescriptor *desc;
	NSError *error = nil;
	uint64_t t0;
	int n;

	if ( mb_pipelines[i].key ) {
		return mb_pipelines[i].object;
	}

	for ( n = 0; n < mb_numPrecache; n++ ) {
		if ( mb_precacheKeys[n] == key && mb_precacheDone[n] ) {
			MB_PrecacheComplete( n );
			i = MB_PipelineSlot( key );
			if ( mb_pipelines[i].key ) {
				return mb_pipelines[i].object;
			}
			break;
		}
	}

	@autoreleasepool {
		t0 = mach_absolute_time();
		desc = MB_PipelineDescriptor( key );
		pipeline = desc ? MB_NewRenderPipeline( desc, &error ) : nil;
		t0 = mach_absolute_time() - t0;
		if ( !pipeline ) {
			if ( desc ) {
				ri.Printf( PRINT_WARNING, "Metal: pipeline 0x%x failed: %s\n", key, error.localizedDescription.UTF8String );
			}
			return NULL;
		}
		MB_PipelineTime( t0 );
		mb_statPipelinesAtDraw++;
		ri.Printf( PRINT_DEVELOPER, "Metal: pipeline 0x%x created at its first draw in %.2f ms\n", key, MB_TicksMs( t0 ) );
		MB_SetPipeline( key, pipeline );
	}
	return mb_pipelines[MB_PipelineSlot( key )].object;
}

// key bits: 0 depth test, 1-3 depth func, 4 depth write, 5 stencil test, 6-8 stencil func, 9-11 fail op,
// 12-14 zfail op, 15-17 zpass op, 18-25 read mask, 26-33 write mask
static uint64_t MB_DepthStencilKey( void )
{
	uint64_t key = 0;

	if ( mb_depthTest ) {
		key |= 1 | ( (uint64_t)MB_CompareIndex( mb_depthFunc ) << 1 ) | ( mb_depthWrite ? 1u << 4 : 0 );
	}
	if ( mb_stencilTest ) {
		key |= ( 1u << 5 ) | ( (uint64_t)MB_CompareIndex( mb_stencilFunc ) << 6 )
			| ( (uint64_t)MB_StencilOpIndex( mb_stencilFail ) << 9 )
			| ( (uint64_t)MB_StencilOpIndex( mb_stencilZFail ) << 12 )
			| ( (uint64_t)MB_StencilOpIndex( mb_stencilZPass ) << 15 )
			| ( (uint64_t)( mb_stencilReadMask & 0xff ) << 18 )
			| ( (uint64_t)( mb_stencilWriteMask & 0xff ) << 26 );
	}
	return key;
}

static void *MB_DepthStencilState( uint64_t key )
{
	uint32_t i = (uint32_t)( ( key * 0x9E3779B97F4A7C15ull ) >> 57 ) & ( MB_DS_SLOTS - 1 );
	MTLDepthStencilDescriptor *desc;
	id<MTLDepthStencilState> state;

	while ( mb_depthStencils[i].key ) {
		if ( mb_depthStencils[i].key == key + 1 ) {
			return mb_depthStencils[i].object;
		}
		i = ( i + 1 ) & ( MB_DS_SLOTS - 1 );
	}

	@autoreleasepool {
		desc = [MTLDepthStencilDescriptor new];
		if ( key & 1 ) {
			desc.depthCompareFunction = (MTLCompareFunction)( ( key >> 1 ) & 7 );
			desc.depthWriteEnabled = ( key >> 4 ) & 1;
		} else {
			// GL: without the depth test nothing is written to depth either
			desc.depthCompareFunction = MTLCompareFunctionAlways;
			desc.depthWriteEnabled = NO;
		}
		if ( key & ( 1u << 5 ) ) {
			MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];

			stencil.stencilCompareFunction = (MTLCompareFunction)( ( key >> 6 ) & 7 );
			stencil.stencilFailureOperation = (MTLStencilOperation)( ( key >> 9 ) & 7 );
			stencil.depthFailureOperation = (MTLStencilOperation)( ( key >> 12 ) & 7 );
			stencil.depthStencilPassOperation = (MTLStencilOperation)( ( key >> 15 ) & 7 );
			stencil.readMask = ( key >> 18 ) & 0xff;
			stencil.writeMask = ( key >> 26 ) & 0xff;
			desc.frontFaceStencil = stencil;
			desc.backFaceStencil = stencil;
		}
		state = [mb_device newDepthStencilStateWithDescriptor:desc];
	}

	mb_depthStencils[i].key = key + 1;
	mb_depthStencils[i].object = (__bridge_retained void *)state;
	return mb_depthStencils[i].object;
}

static int MB_FilterIndex( int filter )
{
	switch ( filter ) {
	case MB_NEAREST:				return 0;
	case MB_LINEAR:					return 1;
	case MB_NEAREST_MIPMAP_NEAREST:	return 2;
	case MB_LINEAR_MIPMAP_NEAREST:	return 3;
	case MB_NEAREST_MIPMAP_LINEAR:	return 4;
	case MB_LINEAR_MIPMAP_LINEAR:	return 5;
	default:						return 1;
	}
}

static void *MB_Sampler( uint32_t key )
{
	uint32_t i = ( key * 2654435761u ) & ( MB_SAMPLER_SLOTS - 1 );
	MTLSamplerDescriptor *desc;
	int min;

	while ( mb_samplers[i].key ) {
		if ( mb_samplers[i].key == key + 1 ) {
			return mb_samplers[i].object;
		}
		i = ( i + 1 ) & ( MB_SAMPLER_SLOTS - 1 );
	}

	@autoreleasepool {
		desc = [MTLSamplerDescriptor new];
		min = key & 7;
		desc.minFilter = ( min & 1 ) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
		desc.mipFilter = min < 2 ? MTLSamplerMipFilterNotMipmapped : ( min < 4 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear );
		desc.magFilter = ( key & 8 ) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
		desc.sAddressMode = ( key & 16 ) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
		desc.tAddressMode = ( key & 32 ) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
		desc.maxAnisotropy = ( ( key >> 6 ) & 31 ) ? ( ( key >> 6 ) & 31 ) : 1;
		desc.lodMinClamp = 0.0f;
		desc.lodMaxClamp = (float)( ( key >> 11 ) & 31 );
		mb_samplers[i].object = (__bridge_retained void *)[mb_device newSamplerStateWithDescriptor:desc];
	}
	mb_samplers[i].key = key + 1;
	return mb_samplers[i].object;
}

static void MB_UpdateSampler( mbTexture_t *t )
{
	uint32_t key;
	int aniso = t->anisotropy < 1 ? 1 : ( t->anisotropy > 16 ? 16 : t->anisotropy );
	int maxLod = t->definedLevels > 1 ? t->definedLevels - 1 : 0;

	key = MB_FilterIndex( t->minFilter )
		| ( t->magFilter == MB_NEAREST ? 0 : 8 )
		| ( t->wrapS == MB_REPEAT ? 0 : 16 )
		| ( t->wrapT == MB_REPEAT ? 0 : 32 )
		| ( aniso << 6 )
		| ( maxLod << 11 );
	t->sampler = MB_Sampler( key );
	t->samplerDirty = qfalse;
}

/*
=============================================================

DEVICE, WINDOW AND FRAME

=============================================================
*/

/*
** MB_CreateSceneTargets
*/
static qboolean MB_CreateSceneTargets( int width, int height )
{
	MTLTextureDescriptor *desc;

	desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		width:width height:height mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;
	mb_sceneColor = [mb_device newTextureWithDescriptor:desc];
	if ( !mb_sceneColor ) {
		return qfalse;
	}
	mb_sceneColor.label = @"scene color";

	desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
		width:width height:height mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget;
	desc.storageMode = MTLStorageModePrivate;
	mb_sceneDepth = [mb_device newTextureWithDescriptor:desc];
	if ( !mb_sceneDepth ) {
		return qfalse;
	}
	mb_sceneDepth.label = @"scene depth";

	mb_width = width;
	mb_height = height;
	mb_sceneColorValid = qfalse;
	mb_depthValid = qfalse;
	return qtrue;
}

/*
** MB_CreatePipelines
*/
static qboolean MB_CreatePipelines( void )
{
	NSError *error = nil;
	dispatch_data_t data;
	MTLRenderPipelineDescriptor *desc;

	if ( !mb_library ) {
		data = dispatch_data_create( tr_mtl_metallib, tr_mtl_metallib_len, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT );
		mb_library = [mb_device newLibraryWithData:data error:&error];
		if ( !mb_library ) {
			ri.Printf( PRINT_ALL, "mbInit: embedded metallib failed to load: %s\n", error.localizedDescription.UTF8String );
			return qfalse;
		}
		mb_stageVS = [mb_library newFunctionWithName:@"mb_stage_vs"];
		mb_clearVS = [mb_library newFunctionWithName:@"mb_clear_vs"];
		mb_clearFS = [mb_library newFunctionWithName:@"mb_clear_fs"];
		if ( !mb_stageVS || !mb_clearVS || !mb_clearFS ) {
			ri.Printf( PRINT_ALL, "mbInit: the embedded metallib lacks the stage shaders\n" );
			mb_library = nil;
			return qfalse;
		}
		mb_skinVS = [mb_library newFunctionWithName:@"mb_skin_vs"];	// GPU skinning is off without it
	}

	if ( !mb_queryPipeline ) {
		// depth queries: a point, depth-tested, no fragment function, nothing written (flares then just never
		// test as visible if this fails)
		desc = [MTLRenderPipelineDescriptor new];
		desc.label = @"depth query";
		desc.vertexFunction = [mb_library newFunctionWithName:@"mb_query_vs"];
		desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
		desc.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
		desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
		desc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
		desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
		mb_queryPipeline = MB_NewRenderPipeline( desc, &error );
		if ( !mb_queryPipeline ) {
			ri.Printf( PRINT_WARNING, "Metal: depth query pipeline failed: %s\n", error.localizedDescription.UTF8String );
		}
	}

	if ( !mb_finalPipeline ) {
		// the layer's pixel format never changes
		desc = [MTLRenderPipelineDescriptor new];
		desc.label = @"final";
		desc.vertexFunction = [mb_library newFunctionWithName:@"mb_final_vs"];
		desc.fragmentFunction = [mb_library newFunctionWithName:@"mb_final_fs"];
		desc.colorAttachments[0].pixelFormat = mb_layer.pixelFormat;
		mb_finalPipeline = MB_NewRenderPipeline( desc, &error );
		if ( !mb_finalPipeline ) {
			ri.Printf( PRINT_ALL, "mbInit: final pipeline failed: %s\n", error.localizedDescription.UTF8String );
			return qfalse;
		}
	}

	return qtrue;
}

/*
** MB_ResetState
**
** GL's initial context state (a new window is a new context for the front end).
*/
static void MB_ResetState( void )
{
	int i;

	mb_blend = qfalse;
	mb_srcBlend = MB_BlendIndex( MB_ONE );
	mb_dstBlend = MB_BlendIndex( MB_ZERO );
	mb_depthTest = qfalse;
	mb_depthFunc = MB_LESS;
	mb_depthWrite = qtrue;
	mb_depthNear = 0.0;
	mb_depthFar = 1.0;
	mb_depthFloor = 0.0f;
	mb_depthClamp = qfalse;
	mb_alphaTest = qfalse;
	mb_alphaFunc = MB_ALWAYS;
	mb_alphaRef = 0.0f;
	mb_cull = qfalse;
	mb_cullFace = MB_BACK;
	mb_fillMode = MB_FILL;
	mb_polygonOffset = qfalse;
	mb_offsetFactor = mb_offsetUnits = 0.0f;
	mb_colorMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
	mb_stencilTest = qfalse;
	mb_stencilFunc = MB_ALWAYS;
	mb_stencilRef = 0;
	mb_stencilReadMask = mb_stencilWriteMask = ~0u;
	mb_stencilFail = mb_stencilZFail = mb_stencilZPass = MB_KEEP;
	mb_scissorTest = qfalse;
	mb_viewport[0] = mb_viewport[1] = mb_scissor[0] = mb_scissor[1] = 0;
	mb_viewport[2] = mb_scissor[2] = mb_width;
	mb_viewport[3] = mb_scissor[3] = mb_height;
	mb_fog = qfalse;
	mb_fogColor[0] = mb_fogColor[1] = mb_fogColor[2] = mb_fogColor[3] = 0.0f;
	mb_fogStart = 0.0f;
	mb_fogEnd = 1.0f;
	mb_clipPlaneOn = qfalse;
	mb_clearColor[0] = mb_clearColor[1] = mb_clearColor[2] = mb_clearColor[3] = 0.0f;
	mb_clearDepth = 1.0;
	mb_clearStencil = 0;

	memset( mb_units, 0, sizeof( mb_units ) );
	for ( i = 0; i < MB_MAX_UNITS; i++ ) {
		mb_units[i].env = MB_ENV_MODULATE;
	}
	mb_activeUnit = mb_clientUnit = 0;
	memset( &mb_vertexArray, 0, sizeof( mb_vertexArray ) );
	memset( &mb_colorArray, 0, sizeof( mb_colorArray ) );
	mb_locked = qfalse;

	mb_matrixMode = MB_MODELVIEW;
	mb_modelviewDepth = mb_projectionDepth = 0;
	MB_Identity( mb_modelview[0] );
	MB_Identity( mb_projection[0] );

	memset( &mb_vsu, 0, sizeof( mb_vsu ) );
	mb_vsu.color[0] = mb_vsu.color[1] = mb_vsu.color[2] = mb_vsu.color[3] = 1.0f;
	mb_vsuTransformValid = qfalse;

	mb_pendingClear = 0;
	mb_dirty = MB_DIRTY_ALL;
}

static void MB_LatencyTime( uint64_t seq, int which, CFTimeInterval t );
static void MB_LatencyNotPresented( uint64_t seq );

/*
** MB_EncodeFinal
**
** The final pass: the scene through the gamma ramp into the drawable, with the presented handlers. Encoded into the
** frame's command buffer, or into a late present's on the drawable thread.
*/
static void MB_EncodeFinal( id<MTLCommandBuffer> cmd, id<CAMetalDrawable> drawable, id<MTLTexture> scene,
	const void *ramp, uint64_t latency )
{
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	id<MTLRenderCommandEncoder> enc;

	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	enc = [cmd renderCommandEncoderWithDescriptor:pass];
	[enc setRenderPipelineState:mb_finalPipeline];
	[enc setFragmentTexture:scene atIndex:0];
	[enc setFragmentBytes:ramp length:sizeof( mb_gammaRamp ) atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[enc endEncoding];
	if ( r_metalPresentStats->integer ) {
		mb_statPresented++;
		[drawable addPresentedHandler:^( id<MTLDrawable> shown ) {
			if ( shown.presentedTime > 0.0 ) {
				mb_statShown++;
			}
		}];
	}
	if ( latency ) {
		[drawable addPresentedHandler:^( id<MTLDrawable> shown ) {
			MB_LatencyTime( latency, MB_LAT_PRESENT, shown.presentedTime );
		}];
	}
}

/*
** MB_PresentLate
**
** A late present gets its drawable: encode the frame's final pass and present (the command buffer runs right after
** the frame's own, before anything the next frame commits).
*/
static void MB_PresentLate( id<MTLCommandBuffer> cmd, id<CAMetalDrawable> drawable, id<MTLTexture> scene,
	const void *ramp, uint64_t latency )
{
	MB_EncodeFinal( cmd, drawable, scene, ramp, latency );
	[cmd presentDrawable:drawable];
	[cmd commit];
	mb_statLate++;
}

/*
** MB_PostLatePresent
**
** The frame just committed found no drawable ready: leave its final pass to the drawable thread. Returns qfalse when
** late presents are off.
*/
static qboolean MB_PostLatePresent( uint64_t latency )
{
	id<MTLCommandBuffer> cmd;
	id<CAMetalDrawable> drawable;

	// A late present is worth it only if it can be on screen, and its drawable back, before the next frame ends:
	// direct to display that takes ~1.7 ms from its commit, plus ~0.25 ms. At 250 fps that leaves 2 ms; at 500 fps
	// and above none, and the next frame keeps the drawable.
	if ( !mb_latePresent || mb_frameInterval <= mb_lateMargin ) {
		return qfalse;
	}

	cmd = [mb_queue commandBuffer];
	cmd.label = @"late present";
	[cmd enqueue];		// its place in the queue: after this frame, before anything later

	os_unfair_lock_lock( &mb_readyLock );
	drawable = mb_readyDrawable;
	mb_readyDrawable = nil;
	if ( !drawable ) {
		mb_lateCmd = cmd;
		mb_lateScene = mb_sceneColor;
		memcpy( mb_lateRamp, mb_gammaRamp, sizeof( mb_lateRamp ) );
		mb_lateLatency = latency;
		mb_lateDeadline = mb_frameEnd + mb_frameInterval - mb_lateMargin;
	}
	os_unfair_lock_unlock( &mb_readyLock );

	if ( drawable ) {
		// it came while the frame was being committed
		dispatch_semaphore_signal( mb_acquireRequest );
		MB_PresentLate( cmd, drawable, mb_sceneColor, mb_gammaRamp, latency );
	}
	return qtrue;
}

/*
** MB_CancelLatePresent
**
** Before anything else is committed: a late present still waiting for its drawable gives way to the newer frame.
*/
static void MB_CancelLatePresent( void )
{
	id<MTLCommandBuffer> cmd;
	uint64_t latency;

	if ( !mb_lateCmd ) {
		return;		// read unlocked: only this thread sets it
	}

	os_unfair_lock_lock( &mb_readyLock );
	cmd = mb_lateCmd;
	latency = mb_lateLatency;
	mb_lateCmd = nil;
	mb_lateScene = nil;
	os_unfair_lock_unlock( &mb_readyLock );

	if ( cmd ) {
		[cmd commit];	// empty: the queue moves on
		if ( latency ) {
			MB_LatencyNotPresented( latency );
		}
	}
}

/*
** MB_AcquireThread
**
** Waits in nextDrawable so the frame never has to (vsync off), and runs late presents.
*/
static void *MB_AcquireThread( void *arg )
{
	CAMetalLayer *layer = (__bridge_transfer CAMetalLayer *)arg;
	qboolean fetch = qfalse;	// a late present used the last drawable: fetch the next one at once

	pthread_setname_np( "openmohaa drawables" );

	while ( 1 ) {
		if ( !fetch ) {
			dispatch_semaphore_wait( mb_acquireRequest, DISPATCH_TIME_FOREVER );
		}
		fetch = qfalse;
		if ( mb_acquireQuit ) {
			break;
		}

		@autoreleasepool {
			id<CAMetalDrawable> drawable = [layer nextDrawable];

			if ( !drawable ) {
				// timed out (hidden window): try again
				dispatch_semaphore_signal( mb_acquireRequest );
				continue;
			}

			id<MTLCommandBuffer> lateCmd;
			id<MTLTexture> lateScene = nil;
			uint8_t lateRamp[256][4];
			uint64_t lateLatency = 0;

			os_unfair_lock_lock( &mb_readyLock );
			lateCmd = mb_lateCmd;
			if ( lateCmd && mach_absolute_time() > mb_lateDeadline ) {
				lateCmd = nil;		// too late: the next frame takes the drawable (and cancels the late present)
			}
			if ( lateCmd ) {
				lateScene = mb_lateScene;
				memcpy( lateRamp, mb_lateRamp, sizeof( lateRamp ) );
				lateLatency = mb_lateLatency;
				mb_lateCmd = nil;
				mb_lateScene = nil;
			} else {
				mb_readyDrawable = drawable;
			}
			os_unfair_lock_unlock( &mb_readyLock );

			if ( lateCmd ) {
				MB_PresentLate( lateCmd, drawable, lateScene, lateRamp, lateLatency );
				fetch = qtrue;
			}
		}
	}

	return NULL;
}

/*
** MB_StartDrawableAcquisition
*/
static qboolean MB_StartDrawableAcquisition( void )
{
	pthread_attr_t attr;
	int err;

	mb_vsync = mb_layer.displaySyncEnabled ? qtrue : qfalse;
	mb_latePresent = !mb_vsync && r_metalLatePresent->integer ? qtrue : qfalse;
	if ( !mb_lateMargin ) {
		mach_timebase_info_data_t tb;

		mach_timebase_info( &tb );
		mb_lateMargin = 2000000ull * tb.denom / tb.numer;	// 2 ms
		mb_wakeIdle = 1000000ull * tb.denom / tb.numer;	// 1 ms
	}
	mb_frameEnd = mb_frameInterval = 0;
	if ( mb_vsync ) {
		return qtrue;
	}

	mb_acquireQuit = qfalse;
	mb_acquireRequest = dispatch_semaphore_create( 0 );

	pthread_attr_init( &attr );
	pthread_attr_set_qos_class_np( &attr, QOS_CLASS_USER_INTERACTIVE, 0 );
	err = pthread_create( &mb_acquireThread, &attr, MB_AcquireThread, (__bridge_retained void *)mb_layer );
	pthread_attr_destroy( &attr );
	if ( err ) {
		ri.Printf( PRINT_ALL, "mbInit: can't start the drawable thread (%d)\n", err );
		mb_acquireRequest = nil;
		return qfalse;
	}
	mb_acquireThreadRunning = qtrue;

	dispatch_semaphore_signal( mb_acquireRequest );	// fetch the first drawable
	return qtrue;
}

/*
** MB_StopDrawableAcquisition
*/
static void MB_StopDrawableAcquisition( void )
{
	if ( mb_acquireThreadRunning ) {
		mb_acquireQuit = qtrue;
		dispatch_semaphore_signal( mb_acquireRequest );
		pthread_join( mb_acquireThread, NULL );
		mb_acquireThreadRunning = qfalse;
	}
	mb_acquireRequest = nil;
	mb_readyDrawable = nil;
	mb_nextDrawable = nil;
	MB_CancelLatePresent();
}

/*
** MB_TakeDrawable
**
** The drawable for this frame's final pass, or nil (vsync off and none ready: skip the present).
*/
static id<CAMetalDrawable> MB_TakeDrawable( void )
{
	id<CAMetalDrawable> drawable;
	uint64_t t0;

	if ( mb_vsync ) {
		drawable = mb_nextDrawable;
		mb_nextDrawable = nil;
		if ( !drawable ) {
			t0 = mach_absolute_time();
			drawable = [mb_layer nextDrawable];
			mb_statDrawableWait += mach_absolute_time() - t0;
		}
		return drawable;
	}

	os_unfair_lock_lock( &mb_readyLock );
	drawable = mb_readyDrawable;
	mb_readyDrawable = nil;
	os_unfair_lock_unlock( &mb_readyLock );

	if ( drawable ) {
		dispatch_semaphore_signal( mb_acquireRequest );	// fetch the next one
	} else {
		mb_statSkipped++;
	}
	return drawable;
}

/*
** MB_WakeCommit
**
** A tiny command buffer that gets the GPU going (mbWakeGPU).
*/
static void MB_WakeCommit( void )
{
	@autoreleasepool {
		id<MTLCommandBuffer> cmd = [mb_queue commandBufferWithUnretainedReferences];
		id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];

		[blit fillBuffer:mb_wakeBuffer range:NSMakeRange( 0, 4 ) value:0];
		[blit endEncoding];
		[cmd commit];
	}
}

/*
** mbInit
**
** Called by MTLimp_Init once the window and its CAMetalLayer exist and glConfig.vidWidth/vidHeight hold
** the drawable size in pixels. The device, queue, shader library and state caches live for the whole
** process (vid_restart keeps them).
*/
qboolean mbInit( void *metalLayer )
{
	int i;

	r_metalFramesInFlight = ri.Cvar_Get( "r_metalFramesInFlight", "2", CVAR_ARCHIVE | CVAR_LATCH );
	r_metalPresentStats = ri.Cvar_Get( "r_metalPresentStats", "0", 0 );
	r_metalLatePresent = ri.Cvar_Get( "r_metalLatePresent", "1", CVAR_LATCH );
	r_metalPipelineCache = ri.Cvar_Get( "r_metalPipelineCache", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_metalWakeGPU = ri.Cvar_Get( "r_metalWakeGPU", "1", 0 );

	if ( !mb_device ) {
		mb_device = MTLCreateSystemDefaultDevice();
		if ( !mb_device ) {
			ri.Printf( PRINT_ALL, "mbInit: no Metal device\n" );
			return qfalse;
		}
		mb_queue = [mb_device newCommandQueue];
		mb_queue.label = @"openmohaa";
		mb_wakeBuffer = [mb_device newBufferWithLength:16 options:MTLResourceStorageModePrivate];
		MB_WakeCommit();	// the first one costs ~0.5 ms or more on the CPU (the driver's first blit): here, not in play
		Com_sprintf( mb_rendererString, sizeof( mb_rendererString ), "%s (Metal)", mb_device.name.UTF8String );
	}

	mb_layer = (__bridge CAMetalLayer *)metalLayer;
	mb_layer.device = mb_device;
	mb_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;	// not sRGB: Q3 math is gamma-space
	mb_layer.framebufferOnly = YES;

	MB_OpenPipelineCache();
	if ( !MB_CreatePipelines() || !MB_CreateSceneTargets( glConfig.vidWidth, glConfig.vidHeight ) ) {
		mbShutdown();
		return qfalse;
	}
	MB_PrewarmPipelines();

	for ( i = 0; i < 256; i++ ) {
		mb_gammaRamp[i][0] = mb_gammaRamp[i][1] = mb_gammaRamp[i][2] = i;
		mb_gammaRamp[i][3] = 255;
	}

	// Frames in flight: mbEndFrame waits, right after committing a frame, until the GPU has finished the frame
	// whose ring slot the next one reuses (never between input sampling and encoding).
	mb_framesInFlight = Com_Clamp( 1, 3, r_metalFramesInFlight->integer );
	if ( !mb_frameEvent ) {
		mb_frameEvent = [mb_device newSharedEvent];
		mb_frameEvent.label = @"frames";
	}

	for ( i = 0; i < mb_framesInFlight; i++ ) {
		mb_ringBuffers[i] = MB_NewRingBuffer( MB_RING_SIZE );
	}
	MB_SelectRing( (int)( mb_frameSerial % mb_framesInFlight ) );

	MB_ResetState();

	if ( !MB_StartDrawableAcquisition() ) {
		mbShutdown();
		return qfalse;
	}

	ri.Printf( PRINT_ALL, "Metal: %s, %dx%d, %d frames in flight, %d drawables, vsync %s\n",
		mb_rendererString, mb_width, mb_height, mb_framesInFlight, (int)mb_layer.maximumDrawableCount,
		mb_layer.displaySyncEnabled ? "on" : "off" );

	return qtrue;
}

/*
** mbShutdown
*/
void mbShutdown( void )
{
	int i;

	// finish what was recorded, then wait for every frame in flight
	MB_EndPass( qfalse );
	MB_CommitUploads();
	if ( mb_cmd ) {
		[mb_cmd commit];
		mb_lastCommitted = mb_cmd;
		mb_cmd = nil;
	}
	if ( mb_lastCommitted ) {
		[mb_lastCommitted waitUntilCompleted];
		mb_lastCommitted = nil;
	}

	// everything has completed: the last query results and readbacks (screenshots taken right before a quit)
	MB_HarvestQueries( UINT64_MAX );
	MB_ProcessReadbacks( UINT64_MAX );

	MB_PrecacheWaitAll();
	MB_SavePipelineCache();
	MB_StopDrawableAcquisition();

	// drop the latency records of this window's frames (their presented handlers may never run)
	os_unfair_lock_lock( &mb_latencyLock );
	mb_latencyTail = mb_latencyHead;
	os_unfair_lock_unlock( &mb_latencyLock );
	mb_latencyTagged = qfalse;

	if ( mb_statFrames && r_metalPresentStats->integer ) {
		mach_timebase_info_data_t tb;

		mach_timebase_info( &tb );
		ri.Printf( PRINT_ALL, "Metal: %d frames, %d presented (%d with no drawable ready at the end of the frame, %d of them "
			"presented late), %d shown; per frame: %.3f ms waiting for a drawable, %.3f ms waiting for a frame slot\n",
			mb_statFrames, (int)mb_statPresented, mb_statSkipped, (int)mb_statLate, (int)mb_statShown,
			(double)mb_statDrawableWait * tb.numer / tb.denom / 1e6 / mb_statFrames,
			(double)mb_statSlotWait * tb.numer / tb.denom / 1e6 / mb_statFrames );
		ri.Printf( PRINT_ALL, "Metal: per frame %.1f draws, %.2f passes, %.2f clear quads, %.1f KB ring; "
			"%d pipelines (%d from the cache, %d compiled ahead of their first draw, %d at it; %.2f ms on the main thread, "
			"slowest %.2f ms), %llu texture copies (in use), %d ring growths\n",
			(double)mb_statDraws / mb_statFrames, (double)mb_statPasses / mb_statFrames,
			(double)mb_statClearQuads / mb_statFrames, (double)mb_statRingBytes / mb_statFrames / 1024.0,
			mb_statPipelines, mb_statPipelinesCached, mb_statPipelinesAhead, mb_statPipelinesAtDraw,
			(double)mb_statPipelineTime * tb.numer / tb.denom / 1e6,
			(double)mb_statPipelineMax * tb.numer / tb.denom / 1e6,
			(unsigned long long)mb_statOrphans, mb_statRingGrowths );
		if ( mb_statGPUFrames ) {
			ri.Printf( PRINT_ALL, "Metal: frame command buffer: %.3f ms GPU time; from commit: %.3f ms to GPU start, %.3f ms to completion (%d frames)\n",
				(double)mb_statGPUTime / 1e6 / mb_statGPUFrames, (double)mb_statGPULatency / 1e6 / mb_statGPUFrames,
				(double)mb_statCompletion / 1e6 / mb_statGPUFrames, (int)mb_statGPUFrames );
		}
		ri.Printf( PRINT_ALL, "Metal: %d GPU wake-ups before a frame (r_metalWakeGPU)\n", mb_statWakes );
	}
	mb_statFrames = mb_statSkipped = 0;
	mb_statLate = 0;
	mb_statPresented = mb_statShown = 0;
	mb_statDrawableWait = mb_statSlotWait = 0;
	mb_statDraws = mb_statPasses = mb_statClearQuads = mb_statOrphans = mb_statRingBytes = 0;
	mb_statGPUTime = mb_statGPULatency = mb_statCompletion = 0;
	mb_statGPUFrames = 0;
	mb_statWakes = 0;
	mb_statRingGrowths = 0;

	mb_staticBuffer = nil;
	mb_staticUsed = 0;
	mb_staticArrays = qfalse;

	// everything has completed: release the deferred objects and the textures
	MB_ReleaseRetired( UINT64_MAX );
	for ( i = 0; i < mb_numPooled; i++ ) {
		MB_RELEASE( mb_texturePool[i].texture );
	}
	mb_numPooled = 0;
	for ( i = 0; i < mb_maxTextures; i++ ) {
		MB_RELEASE( mb_textures[i].texture );
	}
	free( mb_textures );
	mb_textures = NULL;
	mb_maxTextures = 0;
	mb_nextTextureName = 1;
	mb_freeHead = mb_freeTail = -1;
	mb_numFree = 0;
	free( mb_scratch );
	mb_scratch = NULL;
	mb_scratchSize = 0;

	for ( i = 0; i < MB_MAX_FRAMES; i++ ) {
		mb_ringBuffers[i] = nil;
	}
	mb_ring = nil;
	mb_ringBase = NULL;
	mb_ringSize = mb_ringUsed = 0;
	mb_encRing = nil;

	mb_sceneColor = nil;
	mb_sceneDepth = nil;
	mb_layer = nil;
	mb_width = mb_height = 0;
}

const char *mbRendererString( void )
{
	return mb_rendererString;
}

int mbMaxTextureSize( void )
{
	return 16384;	// every Apple GPU family supported by macOS 26
}

/*
** mbSetGamma
*/
void mbSetGamma( const unsigned char red[256], const unsigned char green[256], const unsigned char blue[256] )
{
	int i;

	for ( i = 0; i < 256; i++ ) {
		mb_gammaRamp[i][0] = red[i];
		mb_gammaRamp[i][1] = green[i];
		mb_gammaRamp[i][2] = blue[i];
	}
}

/*
** MB_LatencyUsec
**
** Microseconds on the mach_absolute_time clock, the one presentedTime and NSEvent timestamps use.
*/
static long long MB_LatencyUsec( void )
{
	return (long long)( clock_gettime_nsec_np( CLOCK_UPTIME_RAW ) / 1000 );
}

/*
** MB_LatencyBegin
**
** Starts the record of the frame being ended; returns its sequence number, 0 when the ring is full.
*/
static uint64_t MB_LatencyBegin( int tag )
{
	mbLatencyFrame_t *f;
	uint64_t seq = 0;

	os_unfair_lock_lock( &mb_latencyLock );
	if ( mb_latencyHead - mb_latencyTail < MB_LATENCY_FRAMES ) {
		f = &mb_latency[mb_latencyHead % MB_LATENCY_FRAMES];
		seq = ++mb_latencyHead;
		f->seq = seq;
		f->tag = tag;
		f->pending = MB_LAT_COMMIT | MB_LAT_GPU | MB_LAT_PRESENT;
		f->commitUsec = f->gpuDoneUsec = f->presentUsec = 0;
	}
	os_unfair_lock_unlock( &mb_latencyLock );

	return seq;
}

/*
** MB_LatencyTime
**
** Fills in one of a record's times (host time in seconds, the CACurrentMediaTime clock; 0 = none).
*/
static void MB_LatencyTime( uint64_t seq, int which, CFTimeInterval t )
{
	mbLatencyFrame_t *f = &mb_latency[( seq - 1 ) % MB_LATENCY_FRAMES];

	os_unfair_lock_lock( &mb_latencyLock );
	if ( f->seq == seq && ( f->pending & which ) ) {
		if ( which == MB_LAT_GPU ) {
			f->gpuDoneUsec = (long long)( t * 1e6 );
		} else {
			f->presentUsec = t > 0.0 ? (long long)( t * 1e6 ) : -1;	// 0: Core Animation says never shown
		}
		f->pending &= ~which;
	}
	os_unfair_lock_unlock( &mb_latencyLock );
}

/*
** MB_LatencyNotPresented
**
** A late present gave way to a newer frame: this frame was never presented.
*/
static void MB_LatencyNotPresented( uint64_t seq )
{
	mbLatencyFrame_t *f = &mb_latency[( seq - 1 ) % MB_LATENCY_FRAMES];

	os_unfair_lock_lock( &mb_latencyLock );
	if ( f->seq == seq ) {
		f->pending &= ~MB_LAT_PRESENT;	// presentUsec stays 0
	}
	os_unfair_lock_unlock( &mb_latencyLock );
}

/*
** MB_LatencyCommitted
*/
static void MB_LatencyCommitted( uint64_t seq, qboolean presented )
{
	mbLatencyFrame_t *f = &mb_latency[( seq - 1 ) % MB_LATENCY_FRAMES];
	long long now = MB_LatencyUsec();

	os_unfair_lock_lock( &mb_latencyLock );
	if ( f->seq == seq ) {
		f->commitUsec = now;
		f->pending &= ~MB_LAT_COMMIT;
		if ( !presented ) {
			f->pending &= ~MB_LAT_PRESENT;	// no drawable was ready: presentUsec stays 0
		}
	}
	os_unfair_lock_unlock( &mb_latencyLock );
}

/*
** mbLatencyTag
**
** Called by the client before EndFrame: record the frame under this tag.
*/
void mbLatencyTag( int tag )
{
	mb_latencyTagged = qtrue;
	mb_latencyTag = tag;
}

/*
** mbLatencyPoll
**
** The oldest recorded frame, once its commit, GPU and present times are in.
*/
qboolean mbLatencyPoll( int *tag, long long *commitUsec, long long *gpuDoneUsec, long long *presentUsec )
{
	mbLatencyFrame_t *f;
	qboolean ready = qfalse;

	os_unfair_lock_lock( &mb_latencyLock );
	if ( mb_latencyTail < mb_latencyHead ) {
		f = &mb_latency[mb_latencyTail % MB_LATENCY_FRAMES];
		if ( !( f->pending & MB_LAT_COMMIT ) && f->pending && MB_LatencyUsec() - f->commitUsec > MB_LATENCY_GIVEUP ) {
			if ( f->pending & MB_LAT_PRESENT ) {
				f->presentUsec = -1;	// its presented handler never ran
			}
			f->pending = 0;
		}
		if ( !f->pending ) {
			*tag = f->tag;
			*commitUsec = f->commitUsec;
			*gpuDoneUsec = f->gpuDoneUsec;
			*presentUsec = f->presentUsec;
			mb_latencyTail++;
			ready = qtrue;
		}
	}
	os_unfair_lock_unlock( &mb_latencyLock );

	return ready;
}

/*
** mbEndFrame
*/
void mbEndFrame( void )
{
	uint64_t serial = mb_frameSerial;
	uint64_t latency = 0;		// this frame's cl_latencyStats record

	if ( mb_latencyTagged ) {
		mb_latencyTagged = qfalse;
		if ( mb_layer ) {
			latency = MB_LatencyBegin( mb_latencyTag );
		}
	}

	if ( !mb_layer ) {
		return;
	}

	@autoreleasepool {
		id<CAMetalDrawable> drawable;
		qboolean late;

		// end the scene pass (or run one for clears that nothing drew after, or to initialize the target)
		if ( mb_pendingClear || !mb_sceneColorValid ) {
			MB_BeginPass();
		}
		MB_EndPass( qfalse );
		MB_BeginCommandBuffer();

		// final pass into the drawable, if one is ready
		mb_statFrames++;
		drawable = MB_TakeDrawable();
		if ( mb_latePresent ) {
			// the frame interval (smoothed), for late presents
			uint64_t now = mach_absolute_time();

			if ( mb_frameEnd ) {
				int64_t delta = (int64_t)( now - mb_frameEnd ) - (int64_t)mb_frameInterval;

				mb_frameInterval = mb_frameInterval ? (uint64_t)( (int64_t)mb_frameInterval + delta / 8 ) : now - mb_frameEnd;
			}
			mb_frameEnd = now;
		}
		if ( drawable ) {
			MB_EncodeFinal( mb_cmd, drawable, mb_sceneColor, mb_gammaRamp, latency );
		}

		// Frame completion is read from this event, not from a completed handler: handlers arrived ~1.3 ms after
		// the GPU had finished (and, with a present in the command buffer, only at the display's next refresh
		// while the screen was locked), so the frames-in-flight wait followed the notification, not the GPU.
		[mb_cmd encodeSignalEvent:mb_frameEvent value:serial];
		if ( drawable ) {
			[mb_cmd presentDrawable:drawable];
		}

		if ( r_metalPresentStats->integer ) {
			CFTimeInterval committed = CACurrentMediaTime();

			[mb_cmd addCompletedHandler:^( id<MTLCommandBuffer> buffer ) {
				mb_statGPUTime += (uint64_t)( ( buffer.GPUEndTime - buffer.GPUStartTime ) * 1e9 );
				mb_statGPULatency += (uint64_t)( ( buffer.GPUStartTime - committed ) * 1e9 );
				mb_statCompletion += (uint64_t)( ( CACurrentMediaTime() - committed ) * 1e9 );
				mb_statGPUFrames++;
			}];
		}
		if ( latency ) {
			[mb_cmd addCompletedHandler:^( id<MTLCommandBuffer> buffer ) {
				MB_LatencyTime( latency, MB_LAT_GPU, buffer.GPUEndTime );
			}];
		}
		MB_CommitUploads();
		[mb_cmd commit];
		mb_lastSubmit = mach_absolute_time();
		late = !drawable && MB_PostLatePresent( latency );
		if ( latency ) {
			MB_LatencyCommitted( latency, drawable != nil || late );
		}
		mb_lastCommitted = mb_cmd;
		mb_cmd = nil;
	}

	// Wait for the next frame's ring slot (and with vsync for its drawable) now, at the end of this frame: the
	// waits happen before the engine's frame limiter and input sampling, never between input sampling and present.
	{
		uint64_t t0 = mach_absolute_time();
		uint64_t t1;

		if ( serial + 1 > (uint64_t)mb_framesInFlight ) {
			uint64_t needed = serial + 1 - mb_framesInFlight;

			while ( mb_frameEvent.signaledValue < needed ) {
				if ( ![mb_frameEvent waitUntilSignaledValue:needed timeoutMS:2000] ) {
					ri.Printf( PRINT_WARNING, "Metal: frame %llu has not completed after 2 s\n", (unsigned long long)needed );
				}
			}
		}
		t1 = mach_absolute_time();
		mb_statSlotWait += t1 - t0;

		if ( mb_vsync ) {
			@autoreleasepool {
				mb_nextDrawable = [mb_layer nextDrawable];
			}
			mb_statDrawableWait += mach_absolute_time() - t1;
		}
	}

	// The frame whose slot the next frame reuses has completed: copy its query results out before the slot's
	// buffer is reused, and hand finished readbacks to their callbacks (here, before the next frame's input
	// sampling; nothing waited for them).
	{
		uint64_t completed = mb_frameEvent.signaledValue;

		MB_HarvestQueries( completed );
		if ( mb_queryKeep > 0 ) {
			mb_queryKeep--;
		}
		mb_frameSerial++;
		mb_depthValid = qfalse;		// a new frame starts with depth/stencil cleared, even after a readback
		MB_SelectRing( (int)( mb_frameSerial % mb_framesInFlight ) );
		MB_ReleaseRetired( completed );
		MB_ProcessReadbacks( completed );
	}
}

/*
** mbWakeGPU
**
** Called by the engine when a frame starts, right after its frame limiter's wait and before the input is sampled. The
** GPU powers down after ~1.5 ms without work, and the next commit then waits ~0.3-0.5 ms for it to come back: at 250
** fps every frame did (commit to GPU start ~0.6 ms in play, against ~0.1 ms with the GPU awake). A tiny command buffer
** now has it up when the frame commits, ~0.3-0.6 ms later (a wake-up 0.4-0.8 ms ahead of the commit works best; from
** ~1.4 ms ahead the GPU starts to doze again). Nothing waits for it, and it runs before the frame's work in the queue.
** Skipped when something was committed within the last millisecond (at ~1000 fps the GPU never sleeps).
*/
void mbWakeGPU( void )
{
	uint64_t now;

	if ( !mb_layer || !r_metalWakeGPU->integer ) {
		return;
	}

	now = mach_absolute_time();
	if ( now - mb_lastSubmit < mb_wakeIdle ) {
		return;
	}

	MB_WakeCommit();
	mb_lastSubmit = now;
	mb_statWakes++;
}

/*
** mbFinish
**
** Doesn't wait. GL1 calls glFinish to keep the CPU from running ahead of the GPU (r_finish, every 2D-only
** frame) and before re-uploading cinematic frames; here frames in flight are bounded by r_metalFramesInFlight
** (the wait at the end of the frame) and textures still in use are replaced instead of overwritten, so a wait
** would only add latency between input sampling and present. Readbacks wait for themselves.
*/
void mbFinish( void )
{
}

/*
=============================================================

STATE

=============================================================
*/

static void MB_SetCap( int cap, qboolean on )
{
	switch ( cap ) {
	case MB_BLEND:
		if ( mb_blend != on ) {
			mb_blend = on;
			mb_dirty |= MB_DIRTY_PIPELINE;
		}
		break;
	case MB_DEPTH_TEST:
		if ( mb_depthTest != on ) {
			mb_depthTest = on;
			mb_dirty |= MB_DIRTY_DEPTHSTENCIL;
		}
		break;
	case MB_ALPHA_TEST:
		if ( mb_alphaTest != on ) {
			mb_alphaTest = on;
			mb_dirty |= MB_DIRTY_PIPELINE | MB_DIRTY_FSUNIFORMS;
		}
		break;
	case MB_CULL_FACE:
		if ( mb_cull != on ) {
			mb_cull = on;
			mb_dirty |= MB_DIRTY_RASTER;
		}
		break;
	case MB_POLYGON_OFFSET_FILL:
		if ( mb_polygonOffset != on ) {
			mb_polygonOffset = on;
			mb_dirty |= MB_DIRTY_RASTER;
		}
		break;
	case MB_STENCIL_TEST:
		if ( mb_stencilTest != on ) {
			mb_stencilTest = on;
			mb_dirty |= MB_DIRTY_DEPTHSTENCIL;
		}
		break;
	case MB_SCISSOR_TEST:
		if ( mb_scissorTest != on ) {
			mb_scissorTest = on;
			mb_dirty |= MB_DIRTY_SCISSOR;
		}
		break;
	case MB_FOG:
		if ( mb_fog != on ) {
			mb_fog = on;
			mb_dirty |= MB_DIRTY_FSUNIFORMS;
		}
		break;
	case MB_DEPTH_CLAMP:
		mb_depthClamp = on;		// resolved per draw (MB_PrepareDraw)
		break;
	case MB_CLIP_PLANE0:
		if ( mb_clipPlaneOn != on ) {
			mb_clipPlaneOn = on;
			mb_dirty |= MB_DIRTY_TRANSFORM;
		}
		break;
	case MB_TEXTURE_2D:
		mb_units[mb_activeUnit].enabled = on;	// resolved per draw
		break;
	default:
		// MB_LINE_STIPPLE: lines are drawn solid (debug and editor lines only)
		break;
	}
}

void mbEnable( int cap )
{
	MB_SetCap( cap, qtrue );
}

void mbDisable( int cap )
{
	MB_SetCap( cap, qfalse );
}

void mbBlendFunc( int sfactor, int dfactor )
{
	int src = MB_BlendIndex( sfactor );
	int dst = MB_BlendIndex( dfactor );

	if ( src != mb_srcBlend || dst != mb_dstBlend ) {
		mb_srcBlend = src;
		mb_dstBlend = dst;
		mb_dirty |= MB_DIRTY_PIPELINE;
	}
}

void mbDepthFunc( int func )
{
	if ( mb_depthFunc != func ) {
		mb_depthFunc = func;
		mb_dirty |= MB_DIRTY_DEPTHSTENCIL;
	}
}

void mbDepthMask( qboolean flag )
{
	flag = flag ? qtrue : qfalse;
	if ( mb_depthWrite != flag ) {
		mb_depthWrite = flag;
		mb_dirty |= MB_DIRTY_DEPTHSTENCIL;
	}
}

void mbDepthRange( double zNear, double zFar )
{
	if ( mb_depthNear != zNear || mb_depthFar != zFar ) {
		mb_depthNear = zNear;
		mb_depthFar = zFar;
		mb_dirty |= MB_DIRTY_VIEWPORT;
	}
}

void mbDepthFloor( float floor )
{
	if ( mb_depthFloor != floor ) {
		mb_depthFloor = floor;
		mb_dirty |= MB_DIRTY_VIEWPORT;
	}
}

void mbAlphaFunc( int func, float ref )
{
	if ( mb_alphaFunc != func || mb_alphaRef != ref ) {
		if ( ( mb_alphaFunc == MB_ALWAYS ) != ( func == MB_ALWAYS ) ) {
			mb_dirty |= MB_DIRTY_PIPELINE;
		}
		mb_alphaFunc = func;
		mb_alphaRef = ref;
		mb_dirty |= MB_DIRTY_FSUNIFORMS;
	}
}

void mbCullFace( int mode )
{
	if ( mb_cullFace != mode ) {
		mb_cullFace = mode;
		mb_dirty |= MB_DIRTY_RASTER;
	}
}

void mbPolygonMode( int face, int mode )
{
	if ( mb_fillMode != mode ) {
		mb_fillMode = mode;
		mb_dirty |= MB_DIRTY_RASTER;
	}
}

void mbPolygonOffset( float factor, float units )
{
	if ( mb_offsetFactor != factor || mb_offsetUnits != units ) {
		mb_offsetFactor = factor;
		mb_offsetUnits = units;
		mb_dirty |= MB_DIRTY_RASTER;
	}
}

void mbColorMask( qboolean red, qboolean green, qboolean blue, qboolean alpha )
{
	unsigned mask = ( red ? MTLColorWriteMaskRed : 0 ) | ( green ? MTLColorWriteMaskGreen : 0 ) | ( blue ? MTLColorWriteMaskBlue : 0 );

	if ( mb_colorMask != mask ) {
		mb_colorMask = mask;
		mb_dirty |= MB_DIRTY_PIPELINE;
	}
}

void mbStencilFunc( int func, int ref, unsigned int mask )
{
	if ( mb_stencilFunc != func || mb_stencilReadMask != mask ) {
		mb_stencilFunc = func;
		mb_stencilReadMask = mask;
		mb_dirty |= MB_DIRTY_DEPTHSTENCIL;
	}
	if ( mb_stencilRef != ref ) {
		mb_stencilRef = ref;
		mb_dirty |= MB_DIRTY_STENCILREF;
	}
}

void mbStencilOp( int fail, int zfail, int zpass )
{
	if ( mb_stencilFail != fail || mb_stencilZFail != zfail || mb_stencilZPass != zpass ) {
		mb_stencilFail = fail;
		mb_stencilZFail = zfail;
		mb_stencilZPass = zpass;
		mb_dirty |= MB_DIRTY_DEPTHSTENCIL;
	}
}

void mbStencilMask( unsigned int mask )
{
	if ( mb_stencilWriteMask != mask ) {
		mb_stencilWriteMask = mask;
		mb_dirty |= MB_DIRTY_DEPTHSTENCIL;
	}
}

// smooth shading only (GL1's flat shading is in a disabled REPLACE_MODE path)
void mbShadeModel( int mode ) {}

// Metal draws 1-pixel lines without stipple; only debug views and the editor use other widths
void mbLineWidth( float width ) {}
void mbLineStipple( int factor, unsigned short pattern ) {}

void mbFogf( int pname, float param )
{
	float *value;

	switch ( pname ) {
	case MB_FOG_START:
		value = &mb_fogStart;
		break;
	case MB_FOG_END:
		value = &mb_fogEnd;
		break;
	default:
		return;
	}
	if ( memcmp( value, &param, sizeof( param ) ) ) {
		*value = param;
		mb_dirty |= MB_DIRTY_FSUNIFORMS;
	}
}

void mbFogfv( int pname, const float *params )
{
	if ( pname == MB_FOG_COLOR ) {
		if ( memcmp( mb_fogColor, params, sizeof( mb_fogColor ) ) ) {
			memcpy( mb_fogColor, params, sizeof( mb_fogColor ) );
			mb_dirty |= MB_DIRTY_FSUNIFORMS;
		}
	} else {
		mbFogf( pname, params[0] );
	}
}

// linear fog is the only mode
void mbFogi( int pname, int param ) {}

void mbClipPlane( int plane, const double *equation )
{
	const float *m = mb_modelview[mb_modelviewDepth];
	double inv[16];
	int j;

	// GL stores the plane in eye space: equation * inverse(modelview at the time of the call)
	if ( !MB_InvertMatrix( m, inv ) ) {
		return;
	}
	for ( j = 0; j < 4; j++ ) {
		mb_clipPlaneEye[j] = equation[0] * inv[j * 4 + 0] + equation[1] * inv[j * 4 + 1]
			+ equation[2] * inv[j * 4 + 2] + equation[3] * inv[j * 4 + 3];
	}
	mb_dirty |= MB_DIRTY_TRANSFORM;
}

void mbViewport( int x, int y, int width, int height )
{
	if ( mb_viewport[0] != x || mb_viewport[1] != y || mb_viewport[2] != width || mb_viewport[3] != height ) {
		mb_viewport[0] = x;
		mb_viewport[1] = y;
		mb_viewport[2] = width;
		mb_viewport[3] = height;
		mb_dirty |= MB_DIRTY_VIEWPORT;
	}
}

void mbScissor( int x, int y, int width, int height )
{
	if ( mb_scissor[0] != x || mb_scissor[1] != y || mb_scissor[2] != width || mb_scissor[3] != height ) {
		mb_scissor[0] = x;
		mb_scissor[1] = y;
		mb_scissor[2] = width;
		mb_scissor[3] = height;
		mb_dirty |= MB_DIRTY_SCISSOR;
	}
}

// always the back buffer (no stereo, no front-buffer drawing)
void mbDrawBuffer( int mode ) {}

/*
=============================================================

MATRICES

=============================================================
*/

static float *MB_CurrentMatrix( void )
{
	return mb_matrixMode == MB_PROJECTION ? mb_projection[mb_projectionDepth] : mb_modelview[mb_modelviewDepth];
}

void mbMatrixMode( int mode )
{
	mb_matrixMode = mode;
}

void mbLoadMatrixf( const float *m )
{
	memcpy( MB_CurrentMatrix(), m, 16 * sizeof( float ) );
	mb_dirty |= MB_DIRTY_TRANSFORM;
}

void mbLoadIdentity( void )
{
	MB_Identity( MB_CurrentMatrix() );
	mb_dirty |= MB_DIRTY_TRANSFORM;
}

static void MB_MultiplyCurrent( const float *m )
{
	float *current = MB_CurrentMatrix();
	float result[16];

	MB_MultiplyMatrix( current, m, result );
	memcpy( current, result, sizeof( result ) );
	mb_dirty |= MB_DIRTY_TRANSFORM;
}

void mbOrtho( double left, double right, double bottom, double top, double zNear, double zFar )
{
	float m[16];

	memset( m, 0, sizeof( m ) );
	m[0] = (float)( 2.0 / ( right - left ) );
	m[5] = (float)( 2.0 / ( top - bottom ) );
	m[10] = (float)( -2.0 / ( zFar - zNear ) );
	m[12] = (float)( -( right + left ) / ( right - left ) );
	m[13] = (float)( -( top + bottom ) / ( top - bottom ) );
	m[14] = (float)( -( zFar + zNear ) / ( zFar - zNear ) );
	m[15] = 1.0f;
	MB_MultiplyCurrent( m );
}

void mbTranslatef( float x, float y, float z )
{
	float m[16];

	MB_Identity( m );
	m[12] = x;
	m[13] = y;
	m[14] = z;
	MB_MultiplyCurrent( m );
}

void mbPushMatrix( void )
{
	if ( mb_matrixMode == MB_PROJECTION ) {
		if ( mb_projectionDepth < MB_MATRIX_STACK - 1 ) {
			memcpy( mb_projection[mb_projectionDepth + 1], mb_projection[mb_projectionDepth], 16 * sizeof( float ) );
			mb_projectionDepth++;
		}
	} else if ( mb_modelviewDepth < MB_MATRIX_STACK - 1 ) {
		memcpy( mb_modelview[mb_modelviewDepth + 1], mb_modelview[mb_modelviewDepth], 16 * sizeof( float ) );
		mb_modelviewDepth++;
	}
}

void mbPopMatrix( void )
{
	if ( mb_matrixMode == MB_PROJECTION ) {
		if ( mb_projectionDepth > 0 ) {
			mb_projectionDepth--;
		}
	} else if ( mb_modelviewDepth > 0 ) {
		mb_modelviewDepth--;
	}
	mb_dirty |= MB_DIRTY_TRANSFORM;
}

/*
** MB_UpdateTransform
**
** mvp = projection * modelview; the shader takes GL clip z [-w, w] to Metal's [0, w] after the multiply.
** Folding the remap into the matrix rounds depth differently from GL1 on Apple's GL, so z-fighting where
** models touch the world came out differently (the fixed views: 281 pixels -> 1 on a model standing on a floor).
** The clip plane goes to object space so the shader needs one dot product.
*/
static void MB_UpdateTransform( void )
{
	const float *mv = mb_modelview[mb_modelviewDepth];
	int c;

	// the 2D window and the entities reload the matrices they already have: nothing to recompute then
	if ( mb_vsuTransformValid && mb_vsuClipPlaneOn == mb_clipPlaneOn
		&& !memcmp( mb_vsuModelview, mv, sizeof( mb_vsuModelview ) )
		&& !memcmp( mb_vsuProjection, mb_projection[mb_projectionDepth], sizeof( mb_vsuProjection ) )
		&& ( !mb_clipPlaneOn || !memcmp( mb_vsuClipPlaneEye, mb_clipPlaneEye, sizeof( mb_vsuClipPlaneEye ) ) ) ) {
		return;
	}
	memcpy( mb_vsuModelview, mv, sizeof( mb_vsuModelview ) );
	memcpy( mb_vsuProjection, mb_projection[mb_projectionDepth], sizeof( mb_vsuProjection ) );
	memcpy( mb_vsuClipPlaneEye, mb_clipPlaneEye, sizeof( mb_vsuClipPlaneEye ) );
	mb_vsuClipPlaneOn = mb_clipPlaneOn;
	mb_vsuTransformValid = qtrue;

	MB_MultiplyMatrix( mb_projection[mb_projectionDepth], mv, mb_vsu.mvp );

	mb_vsu.eyeZ[0] = mv[2];
	mb_vsu.eyeZ[1] = mv[6];
	mb_vsu.eyeZ[2] = mv[10];
	mb_vsu.eyeZ[3] = mv[14];

	if ( mb_clipPlaneOn ) {
		for ( c = 0; c < 4; c++ ) {
			mb_vsu.clipPlane[c] = (float)( mb_clipPlaneEye[0] * mv[c * 4 + 0] + mb_clipPlaneEye[1] * mv[c * 4 + 1]
				+ mb_clipPlaneEye[2] * mv[c * 4 + 2] + mb_clipPlaneEye[3] * mv[c * 4 + 3] );
		}
	} else {
		mb_vsu.clipPlane[0] = mb_vsu.clipPlane[1] = mb_vsu.clipPlane[2] = 0.0f;
		mb_vsu.clipPlane[3] = 1.0f;
	}
}

/*
=============================================================

DRAW STATE

=============================================================
*/

static mbTexture_t *MB_TextureForName( unsigned int name )
{
	if ( !name || name >= (unsigned int)mb_maxTextures || !mb_textures[name].allocated ) {
		return NULL;
	}
	return &mb_textures[name];
}

static void MB_ApplyScissor( void )
{
	int x0, y0, x1, y1;

	if ( mb_scissorTest ) {
		// GL's origin is bottom-left, Metal's top-left
		x0 = mb_scissor[0];
		y0 = mb_height - ( mb_scissor[1] + mb_scissor[3] );
		x1 = x0 + mb_scissor[2];
		y1 = y0 + mb_scissor[3];
		if ( x0 < 0 ) x0 = 0;
		if ( y0 < 0 ) y0 = 0;
		if ( x1 > mb_width ) x1 = mb_width;
		if ( y1 > mb_height ) y1 = mb_height;
	} else {
		x0 = y0 = 0;
		x1 = mb_width;
		y1 = mb_height;
	}

	mb_scissorEmpty = ( x1 <= x0 || y1 <= y0 ) ? qtrue : qfalse;
	if ( !mb_scissorEmpty ) {
		[mb_enc setScissorRect:(MTLScissorRect){ (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)( x1 - x0 ), (NSUInteger)( y1 - y0 ) }];
	}
}

static void MB_ApplyViewport( double zNear, double zFar )
{
	[mb_enc setViewport:(MTLViewport){ mb_viewport[0], mb_height - ( mb_viewport[1] + mb_viewport[3] ),
		mb_viewport[2], mb_viewport[3], zNear, zFar }];
}

static void MB_ApplyRaster( void )
{
	MTLCullMode cull = MTLCullModeNone;

	if ( mb_cull ) {
		cull = mb_cullFace == MB_FRONT ? MTLCullModeFront : MTLCullModeBack;
	}
	[mb_enc setCullMode:cull];
	[mb_enc setTriangleFillMode:mb_fillMode == MB_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
	if ( mb_polygonOffset && mb_fillMode != MB_LINE ) {
		[mb_enc setDepthBias:mb_offsetUnits slopeScale:mb_offsetFactor clamp:0.0f];
	} else {
		[mb_enc setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
	}
}

/*
** MB_PrepareDraw
**
** Applies the GL state to the encoder before a draw: viewport, scissor, raster and depth/stencil state,
** the sampled textures, the pipeline and the fragment uniforms. Returns qfalse if nothing can be drawn.
** The caller copies the vertices afterwards, then calls MB_Draw*.
*/
static qboolean MB_PrepareDraw( void )
{
	uint32_t texMask = 0, texFlags = 0;
	int u, numUnits;

	if ( !MB_BeginPass() ) {
		return qfalse;
	}

	if ( mb_dirty & MB_DIRTY_SCISSOR ) {
		MB_ApplyScissor();
	}
	if ( mb_scissorEmpty ) {
		return qfalse;
	}
	if ( mb_dirty & MB_DIRTY_VIEWPORT ) {
		MB_ApplyViewport( mb_depthNear == 0.0 && mb_depthFar == 1.0 ? mb_depthFloor : mb_depthNear, mb_depthFar );
	}
	if ( mb_dirty & MB_DIRTY_RASTER ) {
		MB_ApplyRaster();
	}
	if ( mb_depthClamp || mb_encDepthClamp ) {
		// clamped only with the full depth range, as r_depthClamp asks (the view weapon's and the sky's clip)
		qboolean clamp = mb_depthClamp && mb_depthNear == 0.0 && mb_depthFar == 1.0;

		if ( clamp != mb_encDepthClamp ) {
			[mb_enc setDepthClipMode:clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
			mb_encDepthClamp = clamp;
		}
	}
	if ( mb_dirty & MB_DIRTY_STENCILREF ) {
		[mb_enc setStencilReferenceValue:(uint32_t)mb_stencilRef & 0xff];
	}
	if ( mb_dirty & MB_DIRTY_DEPTHSTENCIL ) {
		void *state = MB_DepthStencilState( MB_DepthStencilKey() );

		if ( state != mb_encDepthStencil ) {
			[mb_enc setDepthStencilState:MB_OBJ( state )];
			mb_encDepthStencil = state;
		}
	}

	// textures: a unit samples if texturing is enabled on it and its name has an image (GL: an incomplete
	// texture disables the unit)
	numUnits = mbMultitexture ? MB_MAX_UNITS : 1;
	for ( u = 0; u < numUnits; u++ ) {
		mbTexture_t *t;

		if ( !mb_units[u].enabled ) {
			continue;
		}
		t = MB_TextureForName( mb_units[u].texture );
		if ( !t || !t->texture ) {
			continue;
		}

		if ( t->samplerDirty ) {
			MB_UpdateSampler( t );
		}
		if ( mb_encTexture[u] != t->texture ) {
			[mb_enc setFragmentTexture:MB_OBJ( t->texture ) atIndex:u];
			mb_encTexture[u] = t->texture;
		}
		if ( mb_encSampler[u] != t->sampler ) {
			[mb_enc setFragmentSamplerState:MB_OBJ( t->sampler ) atIndex:u];
			mb_encSampler[u] = t->sampler;
		}
		t->usedSerial = mb_frameSerial;

		texMask |= 1u << u;
		if ( t->rgbOnly ) {
			texFlags |= MB_FS_RGB0 << u;
		}
	}
	if ( texMask != mb_texMask ) {
		mb_texMask = texMask;
		mb_dirty |= MB_DIRTY_PIPELINE;
	}
	if ( texFlags != mb_texFlags ) {
		mb_texFlags = texFlags;
		mb_dirty |= MB_DIRTY_FSUNIFORMS;
	}

	if ( mb_dirty & MB_DIRTY_PIPELINE ) {
		uint32_t key = MB_PipelineKey();

		if ( key != mb_encPipelineKey ) {
			void *pipeline = MB_Pipeline( key );

			if ( !pipeline ) {
				return qfalse;
			}
			[mb_enc setRenderPipelineState:MB_OBJ( pipeline )];
			mb_encPipelineKey = key;
		}
	}

	if ( mb_dirty & MB_DIRTY_FSUNIFORMS ) {
		mbFSUniforms_t fsu;
		float range = mb_fogEnd - mb_fogStart;

		memcpy( fsu.fogColor, mb_fogColor, sizeof( fsu.fogColor ) );
		fsu.fogEnd = mb_fogEnd;
		fsu.fogScale = range != 0.0f ? 1.0f / range : 1e30f;
		fsu.alphaRef = mb_alphaRef;
		fsu.alphaFunc = MB_CompareIndex( mb_alphaFunc );
		fsu.env0 = mb_units[0].env;
		fsu.env1 = mb_units[1].env;
		fsu.flags = mb_texFlags | ( mb_fog ? MB_FS_FOG : 0 );
		fsu.pad = 0;
		if ( !mb_encFSUValid || memcmp( &mb_encFSU, &fsu, sizeof( fsu ) ) ) {
			[mb_enc setFragmentBytes:&fsu length:sizeof( fsu ) atIndex:0];
			mb_encFSU = fsu;
			mb_encFSUValid = qtrue;
		}
	}

	if ( mb_dirty & MB_DIRTY_TRANSFORM ) {
		MB_UpdateTransform();
	}

	mb_dirty = 0;
	return qtrue;
}

// after the vertices were copied (the ring may have grown since MB_PrepareDraw); streams: the buffer the
// stream offsets point into (the ring, or the static buffer)
static void MB_BindVertices( id<MTLBuffer> streams )
{
	if ( mb_encRing != streams ) {
		[mb_enc setVertexBuffer:streams offset:0 atIndex:0];
		mb_encRing = streams;
	}
	if ( mb_skinDraw && mb_encSkin != mb_skinBuffer ) {
		[mb_enc setVertexBuffer:mb_skinBuffer offset:0 atIndex:2];
		mb_encSkin = mb_skinBuffer;
	}
	[mb_enc setVertexBytes:&mb_vsu length:sizeof( mb_vsu ) atIndex:1];
	mb_statDraws++;
}

static MTLPrimitiveType MB_PrimitiveType( int mode )
{
	switch ( mode ) {
	case MB_POINTS:			return MTLPrimitiveTypePoint;
	case MB_LINES:			return MTLPrimitiveTypeLine;
	case MB_LINE_STRIP:		return MTLPrimitiveTypeLineStrip;
	case MB_TRIANGLE_STRIP:	return MTLPrimitiveTypeTriangleStrip;
	default:				return MTLPrimitiveTypeTriangle;
	}
}

/*
=============================================================

CLEARS

=============================================================
*/

void mbClearColor( float red, float green, float blue, float alpha )
{
	mb_clearColor[0] = red;
	mb_clearColor[1] = green;
	mb_clearColor[2] = blue;
	mb_clearColor[3] = alpha;
}

void mbClearDepth( double depth )
{
	mb_clearDepth = depth;
}

void mbClearStencil( int s )
{
	mb_clearStencil = s;
}

/*
** MB_ClearQuad
**
** A clear inside the pass: one triangle over the viewport-independent scissor rectangle, writing the clear
** values through the color/depth/stencil write masks (as glClear does).
*/
static void MB_ClearQuad( unsigned int mask )
{
	uint32_t key;
	uint64_t dsKey;
	void *pipeline, *state;
	float depth = (float)mb_clearDepth;
	float color[4];
	int saved[4];

	if ( !MB_BeginPass() ) {
		return;
	}
	if ( mb_dirty & MB_DIRTY_SCISSOR ) {
		MB_ApplyScissor();
		mb_dirty &= ~MB_DIRTY_SCISSOR;
	}
	if ( mb_scissorEmpty ) {
		return;
	}

	key = MB_PIPE_CLEAR | ( ( mask & MB_COLOR_BUFFER_BIT ) ? mb_colorMask << 12 : 0 );
	pipeline = MB_Pipeline( key );
	if ( !pipeline ) {
		return;
	}

	// depth always passes; stencil always passes and replaces
	dsKey = 1 | ( (uint64_t)MTLCompareFunctionAlways << 1 ) | ( ( mask & MB_DEPTH_BUFFER_BIT ) ? 1u << 4 : 0 );
	if ( mask & MB_STENCIL_BUFFER_BIT ) {
		dsKey |= ( 1u << 5 ) | ( (uint64_t)MTLCompareFunctionAlways << 6 )
			| ( (uint64_t)MTLStencilOperationReplace << 9 )
			| ( (uint64_t)MTLStencilOperationReplace << 12 )
			| ( (uint64_t)MTLStencilOperationReplace << 15 )
			| ( (uint64_t)0xff << 18 )
			| ( (uint64_t)( mb_stencilWriteMask & 0xff ) << 26 );
	}
	state = MB_DepthStencilState( dsKey );

	// the full target as the viewport, depth range [0, 1]
	memcpy( saved, mb_viewport, sizeof( saved ) );
	mb_viewport[0] = mb_viewport[1] = 0;
	mb_viewport[2] = mb_width;
	mb_viewport[3] = mb_height;
	MB_ApplyViewport( 0.0, 1.0 );
	memcpy( mb_viewport, saved, sizeof( saved ) );

	color[0] = mb_clearColor[0];
	color[1] = mb_clearColor[1];
	color[2] = mb_clearColor[2];
	color[3] = 1.0f;

	[mb_enc setRenderPipelineState:MB_OBJ( pipeline )];
	[mb_enc setDepthStencilState:MB_OBJ( state )];
	[mb_enc setStencilReferenceValue:(uint32_t)mb_clearStencil & 0xff];
	[mb_enc setCullMode:MTLCullModeNone];
	[mb_enc setTriangleFillMode:MTLTriangleFillModeFill];
	[mb_enc setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
	[mb_enc setVertexBytes:&depth length:sizeof( depth ) atIndex:1];
	[mb_enc setFragmentBytes:color length:sizeof( color ) atIndex:0];
	[mb_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	mb_statClearQuads++;

	// the draw state must be set again
	mb_encPipelineKey = 0xffffffff;
	mb_encDepthStencil = NULL;
	mb_encFSUValid = qfalse;
	mb_dirty |= MB_DIRTY_VIEWPORT | MB_DIRTY_RASTER | MB_DIRTY_DEPTHSTENCIL | MB_DIRTY_STENCILREF
		| MB_DIRTY_PIPELINE | MB_DIRTY_FSUNIFORMS;
}

void mbClear( unsigned int mask )
{
	qboolean full;

	mask &= MB_COLOR_BUFFER_BIT | MB_DEPTH_BUFFER_BIT | MB_STENCIL_BUFFER_BIT;
	// the write masks apply to clears
	if ( !mb_colorMask ) {
		mask &= ~MB_COLOR_BUFFER_BIT;
	}
	if ( !mb_depthWrite ) {
		mask &= ~MB_DEPTH_BUFFER_BIT;
	}
	if ( !( mb_stencilWriteMask & 0xff ) ) {
		mask &= ~MB_STENCIL_BUFFER_BIT;
	}
	if ( !mask || !mb_sceneColor ) {
		return;
	}

	// before the pass begins, a clear of the whole target becomes its load action
	full = !mb_scissorTest || ( mb_scissor[0] <= 0 && mb_scissor[1] <= 0
		&& mb_scissor[0] + mb_scissor[2] >= mb_width && mb_scissor[1] + mb_scissor[3] >= mb_height );
	if ( !mb_enc && full
		&& ( !( mask & MB_COLOR_BUFFER_BIT ) || mb_colorMask == ( MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue ) )
		&& ( !( mask & MB_STENCIL_BUFFER_BIT ) || ( mb_stencilWriteMask & 0xff ) == 0xff ) ) {
		if ( mask & MB_COLOR_BUFFER_BIT ) {
			memcpy( mb_pendingClearColor, mb_clearColor, sizeof( mb_pendingClearColor ) );
		}
		if ( mask & MB_DEPTH_BUFFER_BIT ) {
			mb_pendingClearDepth = mb_clearDepth;
		}
		if ( mask & MB_STENCIL_BUFFER_BIT ) {
			mb_pendingClearStencil = mb_clearStencil;
		}
		mb_pendingClear |= mask;
		return;
	}

	MB_ClearQuad( mask );
}

/*
=============================================================

TEXTURES

=============================================================
*/

static int MB_FullMipCount( int width, int height )
{
	int size = width > height ? width : height;
	int levels = 1;

	while ( size > 1 ) {
		size >>= 1;
		levels++;
	}
	return levels;
}

static id<MTLTexture> MB_NewTexture( MTLPixelFormat format, int width, int height, int levels, qboolean rgbOnly )
{
	MTLTextureDescriptor *desc;
	id<MTLTexture> texture;

	@autoreleasepool {
		desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
		desc.mipmapLevelCount = levels;
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;
		if ( rgbOnly ) {
			desc.swizzle = MTLTextureSwizzleChannelsMake( MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne );
		}
		texture = [mb_device newTextureWithDescriptor:desc];
	}
	return texture;
}

static qboolean MB_SameTexture( id<MTLTexture> a, id<MTLTexture> b )
{
	MTLTextureSwizzleChannels sa = a.swizzle, sb = b.swizzle;

	return a.width == b.width && a.height == b.height && a.mipmapLevelCount == b.mipmapLevelCount
		&& a.pixelFormat == b.pixelFormat && sa.alpha == sb.alpha;
}

/*
** MB_ReplaceTexture
**
** The name's texture is still read by a frame in flight (or by this frame): give the name a new texture
** (reused from the pool when one is free). copy: the new texture starts with the old contents (GPU copy).
*/
static void MB_ReplaceTexture( mbTexture_t *t, qboolean copy )
{
	id<MTLTexture> old = (__bridge_transfer id<MTLTexture>)t->texture;
	id<MTLTexture> fresh = nil;
	uint64_t completed = mb_frameEvent.signaledValue;
	int i;

	for ( i = 0; i < mb_numPooled; i++ ) {
		if ( mb_texturePool[i].serial <= completed && MB_SameTexture( MB_OBJ( mb_texturePool[i].texture ), old ) ) {
			fresh = (__bridge_transfer id<MTLTexture>)mb_texturePool[i].texture;
			mb_texturePool[i] = mb_texturePool[--mb_numPooled];
			break;
		}
	}
	if ( !fresh ) {
		fresh = MB_NewTexture( old.pixelFormat, (int)old.width, (int)old.height, (int)old.mipmapLevelCount, t->rgbOnly );
	}

	t->blitSerial = 0;
	if ( copy ) {
		[MB_UploadBlit() copyFromTexture:old toTexture:fresh];
		t->blitSerial = mb_frameSerial;
	}

	// keep the old one for the next replacement once its frames are done
	if ( mb_numPooled == MB_TEXTURE_POOL ) {
		MB_Retire( (__bridge_transfer id)mb_texturePool[0].texture );
		mb_texturePool[0] = mb_texturePool[--mb_numPooled];
	}
	mb_texturePool[mb_numPooled].texture = (__bridge_retained void *)old;
	mb_texturePool[mb_numPooled].serial = mb_frameSerial;
	mb_numPooled++;

	t->texture = (__bridge_retained void *)fresh;
	t->usedSerial = 0;
	mb_statOrphans++;
}

/*
** MB_WriteTexture
**
** Writes one region of one level. fullImage: the write covers the whole texture (one level, all of it),
** so a replaced texture needs no copy of the old contents.
*/
static void MB_WriteTexture( mbTexture_t *t, int level, int x, int y, int width, int height,
	const void *bytes, NSUInteger bytesPerRow, NSUInteger byteCount, qboolean fullImage )
{
	uint64_t completed = mb_frameEvent.signaledValue;
	id<MTLTexture> texture;

	if ( t->usedSerial > completed ) {
		MB_ReplaceTexture( t, !fullImage );
	}
	texture = MB_OBJ( t->texture );

	if ( t->blitSerial > completed ) {
		// GPU copies into this texture are pending: write after them, on the GPU
		uint32_t offset;
		byte *staging = MB_RingAlloc( byteCount, &offset );

		memcpy( staging, bytes, byteCount );
		[MB_UploadBlit() copyFromBuffer:mb_ring sourceOffset:offset sourceBytesPerRow:bytesPerRow
			sourceBytesPerImage:byteCount sourceSize:MTLSizeMake( width, height, 1 )
			toTexture:texture destinationSlice:0 destinationLevel:level destinationOrigin:MTLOriginMake( x, y, 0 )];
		t->blitSerial = mb_frameSerial;
	} else {
		[texture replaceRegion:MTLRegionMake2D( x, y, width, height ) mipmapLevel:level withBytes:bytes bytesPerRow:bytesPerRow];
	}
}

static qboolean MB_IsCompressed( MTLPixelFormat format )
{
	return format == MTLPixelFormatBC1_RGBA || format == MTLPixelFormatBC2_RGBA || format == MTLPixelFormatBC3_RGBA;
}

static NSUInteger MB_LevelBytesPerRow( MTLPixelFormat format, int width )
{
	if ( MB_IsCompressed( format ) ) {
		return (NSUInteger)( ( width + 3 ) / 4 ) * ( format == MTLPixelFormatBC1_RGBA ? 8 : 16 );
	}
	return (NSUInteger)width * 4;
}

static NSUInteger MB_LevelBytes( MTLPixelFormat format, int width, int height )
{
	if ( MB_IsCompressed( format ) ) {
		return MB_LevelBytesPerRow( format, width ) * ( ( height + 3 ) / 4 );
	}
	return (NSUInteger)width * height * 4;
}

/*
** MB_GrowMipChain
**
** A texture is created with one level; the first mip upload gives it the full chain (copying what it has).
*/
static void MB_GrowMipChain( mbTexture_t *t )
{
	id<MTLTexture> old = (__bridge_transfer id<MTLTexture>)t->texture;
	id<MTLTexture> fresh;
	uint64_t completed = mb_frameEvent.signaledValue;
	int level;

	t->levels = MB_FullMipCount( t->width, t->height );
	fresh = MB_NewTexture( t->format, t->width, t->height, t->levels, t->rgbOnly );

	for ( level = 0; level < (int)old.mipmapLevelCount && level < t->definedLevels; level++ ) {
		int w = t->width >> level ? t->width >> level : 1;
		int h = t->height >> level ? t->height >> level : 1;

		if ( t->usedSerial > completed || t->blitSerial > completed ) {
			[MB_UploadBlit() copyFromTexture:old sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake( 0, 0, 0 )
				sourceSize:MTLSizeMake( w, h, 1 ) toTexture:fresh destinationSlice:0 destinationLevel:level
				destinationOrigin:MTLOriginMake( 0, 0, 0 )];
			t->blitSerial = mb_frameSerial;
		} else {
			NSUInteger bytesPerRow = MB_LevelBytesPerRow( t->format, w );
			byte *copy = MB_Scratch( MB_LevelBytes( t->format, w, h ) );

			[old getBytes:copy bytesPerRow:bytesPerRow fromRegion:MTLRegionMake2D( 0, 0, w, h ) mipmapLevel:level];
			[fresh replaceRegion:MTLRegionMake2D( 0, 0, w, h ) mipmapLevel:level withBytes:copy bytesPerRow:bytesPerRow];
		}
	}

	if ( t->usedSerial > completed || t->blitSerial > completed ) {
		MB_Retire( old );	// still read by a frame in flight or by this frame's upload copies
	}
	t->texture = (__bridge_retained void *)fresh;
	t->usedSerial = 0;
}

/*
** MB_PixelFormat
**
** Internal formats as GL1 uploads them: everything uncompressed is RGBA8 (the 16-bit request formats of
** r_texturebits 16 are stored at 32 bits); RGB-like formats sample alpha 1 through a swizzle.
*/
static MTLPixelFormat MB_PixelFormat( int internalformat, qboolean *rgbOnly )
{
	*rgbOnly = qfalse;
	switch ( internalformat ) {
	case 3:
	case MB_RGB:
	case MB_RGB5:
	case MB_RGB8:
	case MB_RGB4_S3TC:
		*rgbOnly = qtrue;
		return MTLPixelFormatRGBA8Unorm;
	case MB_COMPRESSED_RGB_S3TC_DXT1_EXT:
		*rgbOnly = qtrue;
		return MTLPixelFormatBC1_RGBA;
	case MB_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		return MTLPixelFormatBC1_RGBA;
	case MB_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		return MTLPixelFormatBC2_RGBA;
	case MB_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		return MTLPixelFormatBC3_RGBA;
	default:
		return MTLPixelFormatRGBA8Unorm;
	}
}

/*
** MB_UnpackRGBA
**
** Client pixels as tightly packed RGBA8 rows (GL's default unpack alignment of 4 for 3-byte formats).
*/
static const void *MB_UnpackRGBA( int width, int height, int format, const void *pixels )
{
	const byte *src;
	byte *dst, *out;
	size_t srcRow;
	int x, y;

	if ( format != MB_RGB && format != MB_BGR ) {
		return pixels;
	}

	srcRow = ( (size_t)width * 3 + 3 ) & ~(size_t)3;
	out = dst = MB_Scratch( (size_t)width * height * 4 );
	for ( y = 0; y < height; y++ ) {
		src = (const byte *)pixels + y * srcRow;
		for ( x = 0; x < width; x++, src += 3, dst += 4 ) {
			if ( format == MB_BGR ) {
				dst[0] = src[2];
				dst[1] = src[1];
				dst[2] = src[0];
			} else {
				dst[0] = src[0];
				dst[1] = src[1];
				dst[2] = src[2];
			}
			dst[3] = 255;
		}
	}
	return out;
}

/*
** MB_BoundTexture
*/
static mbTexture_t *MB_BoundTexture( void )
{
	return MB_TextureForName( mb_units[mb_activeUnit].texture );
}

/*
** MB_DefineLevel
**
** Makes sure the bound texture has a level of this size and format (glTexImage2D semantics). Returns the
** texture to write, or NULL.
*/
static mbTexture_t *MB_DefineLevel( int level, int internalformat, int width, int height )
{
	mbTexture_t *t = MB_BoundTexture();
	MTLPixelFormat format;
	qboolean rgbOnly;

	if ( !t || width <= 0 || height <= 0 || level < 0 ) {
		return NULL;
	}

	format = MB_PixelFormat( internalformat, &rgbOnly );

	if ( level == 0 ) {
		if ( !t->texture || t->width != width || t->height != height || t->format != format || t->rgbOnly != rgbOnly ) {
			// a new image: a new texture (the old one lives until its frames are done)
			if ( t->texture ) {
				MB_Retire( (__bridge_transfer id)t->texture );
				t->texture = NULL;
			}
			t->texture = (__bridge_retained void *)MB_NewTexture( format, width, height, 1, rgbOnly );
			if ( !t->texture ) {
				return NULL;
			}
			t->width = width;
			t->height = height;
			t->format = format;
			t->rgbOnly = rgbOnly;
			t->levels = 1;
			t->definedLevels = 1;
			t->usedSerial = t->blitSerial = 0;
			t->samplerDirty = qtrue;
		}
		return t;
	}

	if ( !t->texture || format != t->format
		|| width != ( t->width >> level ? t->width >> level : 1 ) || height != ( t->height >> level ? t->height >> level : 1 ) ) {
		return NULL;	// not a mip level of the current image
	}
	if ( level >= t->levels ) {
		MB_GrowMipChain( t );
		if ( level >= t->levels ) {
			return NULL;
		}
	}
	if ( level >= t->definedLevels ) {
		t->definedLevels = level + 1;
		t->samplerDirty = qtrue;
	}
	return t;
}

void mbGenTextures( int n, unsigned int *textures )
{
	int i;

	for ( i = 0; i < n; i++ ) {
		unsigned int name;
		mbTexture_t *t;

		if ( mb_numFree > MB_NAME_REUSE_MIN ) {
			name = (unsigned int)mb_freeHead;
			mb_freeHead = mb_textures[name].nextFree;
			if ( mb_freeHead < 0 ) {
				mb_freeTail = -1;
			}
			mb_numFree--;
		} else {
			name = mb_nextTextureName++;
			if ( name >= (unsigned int)mb_maxTextures ) {
				int oldMax = mb_maxTextures;

				mb_maxTextures = mb_maxTextures ? mb_maxTextures * 2 : 4096;
				mb_textures = realloc( mb_textures, mb_maxTextures * sizeof( *mb_textures ) );
				memset( mb_textures + oldMax, 0, ( mb_maxTextures - oldMax ) * sizeof( *mb_textures ) );
			}
		}

		t = &mb_textures[name];
		memset( t, 0, sizeof( *t ) );
		t->allocated = qtrue;
		// GL's defaults
		t->minFilter = MB_NEAREST_MIPMAP_LINEAR;
		t->magFilter = MB_LINEAR;
		t->wrapS = t->wrapT = MB_REPEAT;
		t->anisotropy = 1;
		t->samplerDirty = qtrue;
		t->nextFree = -1;
		textures[i] = name;
	}
}

void mbDeleteTextures( int n, const unsigned int *textures )
{
	int i;

	for ( i = 0; i < n; i++ ) {
		mbTexture_t *t = MB_TextureForName( textures[i] );

		if ( !t ) {
			continue;
		}
		if ( t->texture ) {
			MB_Retire( (__bridge_transfer id)t->texture );
			t->texture = NULL;
		}
		t->allocated = qfalse;

		// The name waits in a queue before reuse, so a stale name in the front end's bind cache rarely meets a
		// new image. Units keep the name bound: GL would unbind it, but a reused name then resolves to the new
		// image instead of to nothing.
		t->nextFree = -1;
		if ( mb_freeTail >= 0 ) {
			mb_textures[mb_freeTail].nextFree = (int)textures[i];
		} else {
			mb_freeHead = (int)textures[i];
		}
		mb_freeTail = (int)textures[i];
		mb_numFree++;
	}
}

void mbActiveTextureARB( int texture )
{
	int unit = texture >= MB_TEXTURE0 ? texture - MB_TEXTURE0 : texture;

	mb_activeUnit = unit < 0 ? 0 : ( unit >= MB_MAX_UNITS ? MB_MAX_UNITS - 1 : unit );
}

void mbClientActiveTextureARB( int texture )
{
	int unit = texture >= MB_TEXTURE0 ? texture - MB_TEXTURE0 : texture;

	mb_clientUnit = unit < 0 ? 0 : ( unit >= MB_MAX_UNITS ? MB_MAX_UNITS - 1 : unit );
}

void mbBindTexture( int target, unsigned int texture )
{
	mb_units[mb_activeUnit].texture = texture;		// resolved per draw
}

void mbTexEnv( int mode )
{
	uint32_t env;

	switch ( mode ) {
	case MB_MODULATE:	env = MB_ENV_MODULATE; break;
	case MB_REPLACE:	env = MB_ENV_REPLACE; break;
	case MB_DECAL:		env = MB_ENV_DECAL; break;
	case MB_ADD:		env = MB_ENV_ADD; break;
	case MB_COMBINE:	env = MB_ENV_COMBINE; break;
	default:
		// MB_COMBINE4_NV (GLS_MULTITEXTURE_ENV): Apple's GL had no NV_texture_env_combine4, so GL1's
		// glTexEnv call failed with an invalid-enum error there and the unit kept its previous mode. Same here.
		return;
	}
	if ( mb_units[mb_activeUnit].env != env ) {
		mb_units[mb_activeUnit].env = env;
		mb_dirty |= MB_DIRTY_FSUNIFORMS;
	}
}

static void MB_TexParameter( int pname, int param )
{
	mbTexture_t *t = MB_BoundTexture();

	if ( !t ) {
		return;
	}
	switch ( pname ) {
	case MB_TEXTURE_MIN_FILTER:
		t->samplerDirty |= t->minFilter != param;
		t->minFilter = param;
		break;
	case MB_TEXTURE_MAG_FILTER:
		t->samplerDirty |= t->magFilter != param;
		t->magFilter = param;
		break;
	case MB_TEXTURE_WRAP_S:
		t->samplerDirty |= t->wrapS != param;
		t->wrapS = param;
		break;
	case MB_TEXTURE_WRAP_T:
		t->samplerDirty |= t->wrapT != param;
		t->wrapT = param;
		break;
	case MB_TEXTURE_MAX_ANISOTROPY_EXT:
		t->samplerDirty |= t->anisotropy != param;
		t->anisotropy = param;
		break;
	default:
		break;
	}
}

void mbTexParameterf( int target, int pname, float param )
{
	MB_TexParameter( pname, (int)param );
}

void mbTexParameteri( int target, int pname, int param )
{
	MB_TexParameter( pname, param );
}

void mbTexImage2D( int target, int level, int internalformat, int width, int height, int border, int format, int type, const void *pixels )
{
	mbTexture_t *t = MB_DefineLevel( level, internalformat, width, height );

	if ( !t || !pixels || MB_IsCompressed( t->format ) ) {
		return;
	}
	pixels = MB_UnpackRGBA( width, height, format, pixels );
	MB_WriteTexture( t, level, 0, 0, width, height, pixels, (NSUInteger)width * 4, (NSUInteger)width * height * 4, t->levels == 1 );
}

void mbTexSubImage2D( int target, int level, int xoffset, int yoffset, int width, int height, int format, int type, const void *pixels )
{
	mbTexture_t *t = MB_BoundTexture();
	int w, h;

	if ( !t || !t->texture || !pixels || level >= t->levels || MB_IsCompressed( t->format ) || width <= 0 || height <= 0 ) {
		return;
	}
	w = t->width >> level ? t->width >> level : 1;
	h = t->height >> level ? t->height >> level : 1;
	if ( xoffset < 0 || yoffset < 0 || xoffset + width > w || yoffset + height > h ) {
		return;
	}
	pixels = MB_UnpackRGBA( width, height, format, pixels );
	MB_WriteTexture( t, level, xoffset, yoffset, width, height, pixels, (NSUInteger)width * 4, (NSUInteger)width * height * 4,
		t->levels == 1 && xoffset == 0 && yoffset == 0 && width == w && height == h );
}

void mbCompressedTexImage2D( int target, int level, int internalformat, int width, int height, int border, int imageSize, const void *data )
{
	mbTexture_t *t = MB_DefineLevel( level, internalformat, width, height );
	NSUInteger bytes;

	if ( !t || !data || !MB_IsCompressed( t->format ) ) {
		return;
	}
	bytes = MB_LevelBytes( t->format, width, height );
	if ( (NSUInteger)imageSize < bytes ) {
		return;
	}
	MB_WriteTexture( t, level, 0, 0, width, height, data, MB_LevelBytesPerRow( t->format, width ), bytes, t->levels == 1 );
}

/*
=============================================================

CLIENT ARRAYS AND INDEXED DRAWS

=============================================================
*/

static mbArray_t *MB_Array( int array )
{
	switch ( array ) {
	case MB_VERTEX_ARRAY:			return &mb_vertexArray;
	case MB_COLOR_ARRAY:			return &mb_colorArray;
	case MB_TEXTURE_COORD_ARRAY:	return &mb_units[mb_clientUnit].texCoords;
	default:						return NULL;
	}
}

void mbEnableClientState( int array )
{
	mbArray_t *a = MB_Array( array );

	if ( a ) {
		a->enabled = qtrue;
	}
}

void mbDisableClientState( int array )
{
	mbArray_t *a = MB_Array( array );

	if ( a ) {
		a->enabled = qfalse;
		a->lockable = qfalse;
	}
}

static void MB_SetPointer( mbArray_t *a, int size, int type, int stride, const void *pointer )
{
	int elementSize = size * ( type == MB_UNSIGNED_BYTE ? 1 : 4 );

	if ( !stride ) {
		stride = elementSize;
	}
	if ( a->pointer != pointer || a->stride != stride || a->size != size || a->type != type ) {
		a->size = size;
		a->type = type;
		a->stride = stride;
		a->pointer = pointer;
		a->lockable = qfalse;	// different data than what the lock covered
		a->snapValid = qfalse;
	}
}

void mbVertexPointer( int size, int type, int stride, const void *pointer )
{
	MB_SetPointer( &mb_vertexArray, size, type, stride, pointer );
}

void mbColorPointer( int size, int type, int stride, const void *pointer )
{
	MB_SetPointer( &mb_colorArray, size, type, stride, pointer );
}

void mbTexCoordPointer( int size, int type, int stride, const void *pointer )
{
	MB_SetPointer( &mb_units[mb_clientUnit].texCoords, size, type, stride, pointer );
}

void mbLockArraysEXT( int first, int count )
{
	int u;

	mb_locked = qtrue;
	mb_lockFirst = first;
	mb_lockCount = count;

	// the arrays enabled now are copied once, at the first draw of the lock
	mb_vertexArray.lockable = mb_vertexArray.enabled;
	mb_vertexArray.snapValid = qfalse;
	mb_colorArray.lockable = mb_colorArray.enabled;
	mb_colorArray.snapValid = qfalse;
	for ( u = 0; u < MB_MAX_UNITS; u++ ) {
		mb_units[u].texCoords.lockable = mb_units[u].texCoords.enabled;
		mb_units[u].texCoords.snapValid = qfalse;
	}
}

void mbUnlockArraysEXT( void )
{
	int u;

	mb_locked = qfalse;
	mb_vertexArray.lockable = mb_vertexArray.snapValid = qfalse;
	mb_colorArray.lockable = mb_colorArray.snapValid = qfalse;
	for ( u = 0; u < MB_MAX_UNITS; u++ ) {
		mb_units[u].texCoords.lockable = mb_units[u].texCoords.snapValid = qfalse;
	}
}

static NSUInteger MB_ArrayBytes( const mbArray_t *a, int numVertexes )
{
	return (NSUInteger)( numVertexes - 1 ) * a->stride + a->size * ( a->type == MB_UNSIGNED_BYTE ? 1 : 4 );
}

static qboolean MB_ArraySnapshotValid( const mbArray_t *a )
{
	return mb_locked && a->lockable && a->snapValid && a->snapGeneration == mb_ringGeneration;
}

/*
** MB_StreamArrays
**
** Copies the arrays this draw reads into the ring (one allocation, so all offsets share one buffer) and
** fills the stream offsets/strides of the vertex uniforms. extraBytes: room after the vertices (indices).
*/
static byte *MB_StreamArrays( int numVertexes, NSUInteger extraBytes, uint32_t *extraOffset )
{
	mbArray_t *arrays[4];
	NSUInteger offsets[4];
	NSUInteger total;
	byte *base;
	uint32_t baseOffset;
	unsigned generation;
	int i;

	// skinning computes the positions, texcoord 0 and (with lights) the colors
	arrays[0] = mb_skinDraw ? NULL : &mb_vertexArray;
	arrays[1] = ( mb_colorArray.enabled && mb_colorArray.pointer && !( mb_skinDraw && mb_skinDraw->numLights >= 0 ) ) ? &mb_colorArray : NULL;
	arrays[2] = ( ( mb_texMask & 1 ) && mb_units[0].texCoords.enabled && mb_units[0].texCoords.pointer && !mb_skinDraw ) ? &mb_units[0].texCoords : NULL;
	arrays[3] = ( ( mb_texMask & 2 ) && mb_units[1].texCoords.enabled && mb_units[1].texCoords.pointer ) ? &mb_units[1].texCoords : NULL;

	for ( ;; ) {
		total = 0;
		for ( i = 0; i < 4; i++ ) {
			if ( arrays[i] && !MB_ArraySnapshotValid( arrays[i] ) ) {
				offsets[i] = total;
				total += ( MB_ArrayBytes( arrays[i], numVertexes ) + MB_RING_ALIGN - 1 ) & ~(NSUInteger)( MB_RING_ALIGN - 1 );
			}
		}
		generation = mb_ringGeneration;
		base = MB_RingAlloc( total + extraBytes, &baseOffset );
		if ( generation == mb_ringGeneration ) {
			break;
		}
		// the ring grew: snapshots in the old buffer are gone, count them again
	}

	for ( i = 0; i < 4; i++ ) {
		mbArray_t *a = arrays[i];

		if ( !a ) {
			mb_vsu.stride[i] = 0;
			continue;
		}
		if ( MB_ArraySnapshotValid( a ) ) {
			mb_vsu.offset[i] = a->snapOffset;
		} else {
			memcpy( base + offsets[i], a->pointer, MB_ArrayBytes( a, numVertexes ) );
			mb_vsu.offset[i] = baseOffset + (uint32_t)offsets[i];
			if ( mb_locked && a->lockable ) {
				a->snapValid = qtrue;
				a->snapOffset = mb_vsu.offset[i];
				a->snapGeneration = mb_ringGeneration;
			}
		}
		mb_vsu.stride[i] = a->stride;
	}
	mb_vsu.posSize = mb_vertexArray.size;
	mb_vsu.colorFloat = mb_colorArray.type == MB_FLOAT;

	*extraOffset = baseOffset + (uint32_t)total;
	return base + total;
}

/*
** MB_DrawStaticElements
**
** mbDrawElements with the arrays in the static buffer: only the indices are copied.
*/
static void MB_DrawStaticElements( int mode, int count, const void *indices )
{
	const mbArray_t *arrays[4];
	uint32_t indexOffset;
	int i;

	arrays[0] = &mb_vertexArray;
	arrays[1] = ( mb_colorArray.enabled && mb_colorArray.pointer ) ? &mb_colorArray : NULL;
	arrays[2] = ( ( mb_texMask & 1 ) && mb_units[0].texCoords.enabled && mb_units[0].texCoords.pointer ) ? &mb_units[0].texCoords : NULL;
	arrays[3] = ( ( mb_texMask & 2 ) && mb_units[1].texCoords.enabled && mb_units[1].texCoords.pointer ) ? &mb_units[1].texCoords : NULL;

	for ( i = 0; i < 4; i++ ) {
		if ( arrays[i] ) {
			mb_vsu.offset[i] = (uint32_t)(uintptr_t)arrays[i]->pointer;
			mb_vsu.stride[i] = arrays[i]->stride;
		} else {
			mb_vsu.stride[i] = 0;
		}
	}
	mb_vsu.posSize = mb_vertexArray.size;
	mb_vsu.colorFloat = mb_colorArray.type == MB_FLOAT;

	memcpy( MB_RingAlloc( (NSUInteger)count * 4, &indexOffset ), indices, (size_t)count * 4 );

	MB_BindVertices( mb_staticBuffer );
	[mb_enc drawIndexedPrimitives:MB_PrimitiveType( mode ) indexCount:count indexType:MTLIndexTypeUInt32
		indexBuffer:mb_ring indexBufferOffset:indexOffset];
}

void mbDrawElements( int mode, int count, int type, const void *indices )
{
	int numVertexes, i;
	uint32_t indexOffset;
	byte *indexDst;

	if ( count <= 0 || !mb_vertexArray.enabled || !mb_vertexArray.pointer || type != MB_UNSIGNED_INT ) {
		return;
	}
	if ( !MB_PrepareDraw() ) {
		return;
	}
	if ( mb_staticArrays ) {
		MB_DrawStaticElements( mode, count, indices );
		return;
	}

	if ( mb_locked ) {
		numVertexes = mb_lockFirst + mb_lockCount;
	} else {
		const unsigned int *idx = indices;
		unsigned int maxIndex = 0;

		for ( i = 0; i < count; i++ ) {
			if ( idx[i] > maxIndex ) {
				maxIndex = idx[i];
			}
		}
		numVertexes = (int)maxIndex + 1;
	}
	if ( numVertexes <= 0 ) {
		return;
	}

	for ( ;; ) {
		unsigned generation = mb_ringGeneration;

		if ( mb_skinDraw ) {
			MB_SkinUniforms();
		}
		indexDst = MB_StreamArrays( numVertexes, (NSUInteger)count * 4, &indexOffset );
		if ( !mb_skinDraw || generation == mb_ringGeneration ) {
			break;
		}
		// the ring grew after the skinning uniforms were copied: copy them again
	}
	memcpy( indexDst, indices, (size_t)count * 4 );

	MB_BindVertices( mb_ring );
	[mb_enc drawIndexedPrimitives:MB_PrimitiveType( mode ) indexCount:count indexType:MTLIndexTypeUInt32
		indexBuffer:mb_ring indexBufferOffset:indexOffset];
}

/*
=============================================================

STATIC VERTEX DATA

=============================================================
*/

int mbStaticData( const void *data, int bytes )
{
	// offset 0 stays unused: an array pointer of 0 would read as no array
	NSUInteger start = ( ( mb_staticUsed ? mb_staticUsed : 16 ) + 15 ) & ~(NSUInteger)15;

	if ( !mb_device || bytes <= 0 ) {
		return -1;
	}
	if ( !mb_staticBuffer || start + bytes > mb_staticBuffer.length ) {
		NSUInteger size = mb_staticBuffer ? mb_staticBuffer.length * 2 : MB_STATIC_BUFFER_SIZE;
		id<MTLBuffer> buffer;

		while ( size < start + bytes ) {
			size *= 2;
		}
		if ( size > MB_STATIC_BUFFER_MAX ) {
			return -1;
		}
		buffer = [mb_device newBufferWithLength:size options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
		if ( !buffer ) {
			return -1;
		}
		buffer.label = @"static vertex data";
		if ( mb_staticBuffer ) {
			// draws already recorded keep the old buffer until their frames are done
			memcpy( buffer.contents, mb_staticBuffer.contents, mb_staticUsed );
			MB_Retire( mb_staticBuffer );
		}
		mb_staticBuffer = buffer;
	}

	memcpy( (byte *)mb_staticBuffer.contents + start, data, (size_t)bytes );
	mb_staticUsed = start + (NSUInteger)bytes;
	return (int)start;
}

void mbStaticReset( void )
{
	MB_Retire( mb_staticBuffer );
	mb_staticBuffer = nil;
	mb_staticUsed = 0;
	mb_staticArrays = qfalse;
}

qboolean mbStaticArrays( qboolean on )
{
	if ( on && !mb_staticBuffer ) {
		return qfalse;
	}
	mb_staticArrays = on ? qtrue : qfalse;
	return qtrue;
}

/*
=============================================================

GPU SKINNING

=============================================================
*/

int mbSkinStatic( const void *data, int bytes )
{
	NSUInteger start = ( mb_skinUsed + 15 ) & ~(NSUInteger)15;

	if ( !mb_device || !mb_skinVS || bytes <= 0 || ( bytes & 3 ) ) {
		return -1;
	}
	if ( !mb_skinBuffer || start + bytes > mb_skinBuffer.length ) {
		NSUInteger size = mb_skinBuffer ? mb_skinBuffer.length * 2 : MB_SKIN_BUFFER_SIZE;
		id<MTLBuffer> buffer;

		while ( size < start + bytes ) {
			size *= 2;
		}
		if ( size > MB_SKIN_BUFFER_MAX ) {
			return -1;
		}
		buffer = [mb_device newBufferWithLength:size options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
		if ( !buffer ) {
			return -1;
		}
		buffer.label = @"skinning data";
		if ( mb_skinBuffer ) {
			// draws already recorded keep the old buffer until their frames are done
			memcpy( buffer.contents, mb_skinBuffer.contents, mb_skinUsed );
			MB_Retire( mb_skinBuffer );
		}
		mb_skinBuffer = buffer;
	}

	memcpy( (byte *)mb_skinBuffer.contents + start, data, (size_t)bytes );
	mb_skinUsed = start + (NSUInteger)bytes;
	return (int)( start / 4 );
}

void mbSkinReset( void )
{
	MB_Retire( mb_skinBuffer );
	mb_skinBuffer = nil;
	mb_skinUsed = 0;
}

qboolean mbSkinBegin( const mbSkinDraw_t *draw )
{
	if ( !mb_skinBuffer || !mb_skinVS || draw->numJobs < 1 || draw->numJobs > MB_SKIN_MAX_JOBS
		|| draw->numLights < -1 || draw->numLights > MB_SKIN_MAX_LIGHTS ) {
		return qfalse;
	}
	mb_skinDraw = draw;
	mb_dirty |= MB_DIRTY_PIPELINE;
	return qtrue;
}

void mbSkinEnd( void )
{
	mb_skinDraw = NULL;
	mb_dirty |= MB_DIRTY_PIPELINE;
}

// the scene's copy of a skinning input in the ring
static uint32_t MB_SkinCopy( const void *data, int bytes )
{
	uint32_t offset;
	int i;

	for ( i = 0; i < mb_numSkinCopies; i++ ) {
		if ( mb_skinCopies[i].data == data && mb_skinCopies[i].bytes == bytes ) {
			return mb_skinCopies[i].offset;
		}
	}
	memcpy( MB_RingAlloc( (NSUInteger)bytes, &offset ), data, (size_t)bytes );
	if ( mb_numSkinCopies < MB_SKIN_COPIES ) {
		mb_skinCopies[mb_numSkinCopies].data = data;
		mb_skinCopies[mb_numSkinCopies].bytes = bytes;
		mb_skinCopies[mb_numSkinCopies].offset = offset;
		mb_numSkinCopies++;
	}
	return offset;
}

/*
** MB_SkinUniforms
**
** The draw's MbSkinUniforms in the ring: the header, the jobs, the lights. The bones and morph weights are
** copied once per scene.
*/
static void MB_SkinUniforms( void )
{
	const mbSkinDraw_t *draw = mb_skinDraw;
	int numLights = draw->numLights > 0 ? draw->numLights : 0;
	NSUInteger lightBytes = (NSUInteger)numLights * 60;	// reallightinfo_t
	mbSkinJobUniforms_t *jobs;
	uint32_t *header;
	uint32_t offset;
	int i;

	for ( ;; ) {
		unsigned generation;

		if ( mb_skinCopySerial != mb_frameSerial || mb_skinCopyGeneration != mb_ringGeneration || mb_skinCopyScene != draw->scene ) {
			mb_numSkinCopies = 0;
			mb_skinCopySerial = mb_frameSerial;
			mb_skinCopyGeneration = mb_ringGeneration;
			mb_skinCopyScene = draw->scene;
		}
		generation = mb_ringGeneration;

		header = (uint32_t *)MB_RingAlloc( 16 + sizeof( *jobs ) * (NSUInteger)draw->numJobs + lightBytes, &offset );
		header[0] = (uint32_t)draw->numJobs;
		header[1] = (uint32_t)draw->numLights;
		memcpy( &header[2], draw->ambient, 4 );
		header[3] = 0;

		jobs = (mbSkinJobUniforms_t *)( header + 4 );
		for ( i = 0; i < draw->numJobs; i++ ) {
			const mbSkinJob_t *job = &draw->jobs[i];

			jobs[i].firstVertex = (uint32_t)job->firstVertex;
			jobs[i].numVertexes = (uint32_t)job->numVertexes;
			jobs[i].surface = (uint32_t)job->surface;
			jobs[i].boneMap = job->boneMap >= 0 ? (uint32_t)job->boneMap : 0xffffffffu;
			jobs[i].bones = MB_SkinCopy( job->bones, job->numBones * 64 );		// skelBoneCache_t
			jobs[i].morphs = job->morphs ? MB_SkinCopy( job->morphs, job->numMorphs * 4 ) : 0xffffffffu;
			jobs[i].scale = job->scale;
			jobs[i].pad = 0;
		}
		if ( lightBytes ) {
			memcpy( jobs + draw->numJobs, draw->lights, lightBytes );
		}

		if ( generation == mb_ringGeneration ) {
			break;
		}
		// the ring grew: everything copied before is in the old buffer
	}
	mb_vsu.skin = offset;
}

/*
=============================================================

IMMEDIATE MODE

=============================================================
*/

typedef struct {
	float	xyz[4];
	float	color[4];
	float	st[MB_MAX_UNITS][2];
} mbImmVertex_t;

static mbImmVertex_t	*mb_imm;
static int				mb_immCount, mb_immMax;
static int				mb_immMode;
static qboolean			mb_inBegin;

void mbBegin( int mode )
{
	mb_immMode = mode;
	mb_immCount = 0;
	mb_inBegin = qtrue;
}

static void MB_ImmVertex( float x, float y, float z )
{
	mbImmVertex_t *v;

	if ( !mb_inBegin ) {
		return;
	}
	if ( mb_immCount == mb_immMax ) {
		mb_immMax = mb_immMax ? mb_immMax * 2 : 4096;
		mb_imm = realloc( mb_imm, mb_immMax * sizeof( *mb_imm ) );
	}
	v = &mb_imm[mb_immCount++];
	v->xyz[0] = x;
	v->xyz[1] = y;
	v->xyz[2] = z;
	v->xyz[3] = 1.0f;
	memcpy( v->color, mb_vsu.color, sizeof( v->color ) );
	v->st[0][0] = mb_vsu.texCoord[0];
	v->st[0][1] = mb_vsu.texCoord[1];
	v->st[1][0] = mb_vsu.texCoord[2];
	v->st[1][1] = mb_vsu.texCoord[3];
}

void mbEnd( void )
{
	MTLPrimitiveType prim;
	int n = mb_immCount;
	int numIndexes = 0;
	uint32_t offset, *idx;
	byte *base;
	int i;

	if ( !mb_inBegin ) {
		return;
	}
	mb_inBegin = qfalse;

	switch ( mb_immMode ) {
	case MB_POINTS:
		return;		// unused, and Metal points need a point size output
	case MB_LINES:
		prim = MTLPrimitiveTypeLine;
		n &= ~1;
		break;
	case MB_LINE_STRIP:
		prim = MTLPrimitiveTypeLineStrip;
		break;
	case MB_LINE_LOOP:
		prim = MTLPrimitiveTypeLineStrip;
		numIndexes = n > 1 ? n + 1 : 0;
		break;
	case MB_TRIANGLES:
		prim = MTLPrimitiveTypeTriangle;
		n -= n % 3;
		break;
	case MB_TRIANGLE_STRIP:
		prim = MTLPrimitiveTypeTriangleStrip;
		break;
	case MB_QUADS:
		prim = MTLPrimitiveTypeTriangle;
		numIndexes = ( n / 4 ) * 6;
		break;
	default:	// MB_TRIANGLE_FAN, MB_POLYGON
		prim = MTLPrimitiveTypeTriangle;
		numIndexes = n > 2 ? ( n - 2 ) * 3 : 0;
		break;
	}
	if ( n < 2 || ( prim != MTLPrimitiveTypeLine && prim != MTLPrimitiveTypeLineStrip && n < 3 ) ) {
		return;
	}
	if ( numIndexes == 0 && ( mb_immMode == MB_QUADS || mb_immMode == MB_LINE_LOOP || mb_immMode == MB_POLYGON || mb_immMode == MB_TRIANGLE_FAN ) ) {
		return;
	}

	if ( !MB_PrepareDraw() ) {
		return;
	}

	base = MB_RingAlloc( (NSUInteger)n * sizeof( mbImmVertex_t ) + (NSUInteger)numIndexes * 4, &offset );
	memcpy( base, mb_imm, (size_t)n * sizeof( mbImmVertex_t ) );
	idx = (uint32_t *)( base + (size_t)n * sizeof( mbImmVertex_t ) );

	switch ( mb_immMode ) {
	case MB_LINE_LOOP:
		for ( i = 0; i < n; i++ ) {
			idx[i] = i;
		}
		idx[n] = 0;
		break;
	case MB_QUADS:
		for ( i = 0; i + 3 < n; i += 4, idx += 6 ) {
			idx[0] = i;
			idx[1] = i + 1;
			idx[2] = i + 2;
			idx[3] = i;
			idx[4] = i + 2;
			idx[5] = i + 3;
		}
		break;
	case MB_POLYGON:
	case MB_TRIANGLE_FAN:
		for ( i = 1; i + 1 < n; i++, idx += 3 ) {
			idx[0] = 0;
			idx[1] = i;
			idx[2] = i + 1;
		}
		break;
	default:
		break;
	}

	// streams: everything from the immediate vertices, colors as floats
	mb_vsu.offset[0] = offset + offsetof( mbImmVertex_t, xyz );
	mb_vsu.offset[1] = offset + offsetof( mbImmVertex_t, color );
	mb_vsu.offset[2] = offset + offsetof( mbImmVertex_t, st[0] );
	mb_vsu.offset[3] = offset + offsetof( mbImmVertex_t, st[1] );
	mb_vsu.stride[0] = mb_vsu.stride[1] = sizeof( mbImmVertex_t );
	mb_vsu.stride[2] = ( mb_texMask & 1 ) ? sizeof( mbImmVertex_t ) : 0;
	mb_vsu.stride[3] = ( mb_texMask & 2 ) ? sizeof( mbImmVertex_t ) : 0;
	mb_vsu.posSize = 4;
	mb_vsu.colorFloat = 1;

	MB_BindVertices( mb_ring );
	if ( numIndexes ) {
		[mb_enc drawIndexedPrimitives:prim indexCount:numIndexes indexType:MTLIndexTypeUInt32
			indexBuffer:mb_ring indexBufferOffset:offset + (NSUInteger)n * sizeof( mbImmVertex_t )];
	} else {
		[mb_enc drawPrimitives:prim vertexStart:0 vertexCount:n];
	}
}

/*
** mbArrayElement
**
** One vertex from the client arrays inside mbBegin/mbEnd (r_primitives 1); updates the current values like GL.
*/
void mbArrayElement( int i )
{
	const float *p;
	int u;

	if ( !mb_inBegin || !mb_vertexArray.enabled || !mb_vertexArray.pointer ) {
		return;
	}
	if ( mb_colorArray.enabled && mb_colorArray.pointer ) {
		if ( mb_colorArray.type == MB_FLOAT ) {
			memcpy( mb_vsu.color, mb_colorArray.pointer + (size_t)i * mb_colorArray.stride, 4 * sizeof( float ) );
		} else {
			mbColor4ubv( mb_colorArray.pointer + (size_t)i * mb_colorArray.stride );
		}
	}
	for ( u = 0; u < MB_MAX_UNITS; u++ ) {
		const mbArray_t *a = &mb_units[u].texCoords;

		if ( a->enabled && a->pointer ) {
			p = (const float *)( a->pointer + (size_t)i * a->stride );
			mb_vsu.texCoord[u * 2 + 0] = p[0];
			mb_vsu.texCoord[u * 2 + 1] = p[1];
		}
	}
	p = (const float *)( mb_vertexArray.pointer + (size_t)i * mb_vertexArray.stride );
	MB_ImmVertex( p[0], p[1], mb_vertexArray.size > 2 ? p[2] : 0.0f );
}

void mbVertex2f( float x, float y )
{
	MB_ImmVertex( x, y, 0.0f );
}

void mbVertex3f( float x, float y, float z )
{
	MB_ImmVertex( x, y, z );
}

void mbVertex3fv( const float *v )
{
	MB_ImmVertex( v[0], v[1], v[2] );
}

void mbTexCoord2f( float s, float t )
{
	mb_vsu.texCoord[mb_clientUnit * 2 + 0] = s;
	mb_vsu.texCoord[mb_clientUnit * 2 + 1] = t;
}

void mbTexCoord2fv( const float *v )
{
	mbTexCoord2f( v[0], v[1] );
}

void mbMultiTexCoord2fARB( int target, float s, float t )
{
	int unit = target >= MB_TEXTURE0 ? target - MB_TEXTURE0 : target;	// GL1's r_primitives 3 passes 0/1

	if ( unit >= 0 && unit < MB_MAX_UNITS ) {
		mb_vsu.texCoord[unit * 2 + 0] = s;
		mb_vsu.texCoord[unit * 2 + 1] = t;
	}
}

void mbColor3f( float red, float green, float blue )
{
	mbColor4f( red, green, blue, 1.0f );
}

void mbColor4f( float red, float green, float blue, float alpha )
{
	mb_vsu.color[0] = red;
	mb_vsu.color[1] = green;
	mb_vsu.color[2] = blue;
	mb_vsu.color[3] = alpha;
}

void mbColor4ubv( const byte *v )
{
	mbColor4f( v[0] * ( 1.0f / 255.0f ), v[1] * ( 1.0f / 255.0f ), v[2] * ( 1.0f / 255.0f ), v[3] * ( 1.0f / 255.0f ) );
}

/*
=============================================================

READBACK

=============================================================
*/

/*
** MB_ReadbackStart
**
** Makes the scene target hold everything recorded so far and ends the pass, so a blit can read it. The frame
** continues in a new pass that loads color and depth/stencil.
*/
static qboolean MB_ReadbackStart( int format )
{
	if ( !mb_sceneColor ) {
		return qfalse;
	}
	if ( mb_pendingClear || ( format == MB_STENCIL_INDEX ? !mb_depthValid : !mb_sceneColorValid ) ) {
		MB_BeginPass();		// apply the clears (or define the contents) before reading
	}
	MB_EndPass( qtrue );
	MB_BeginCommandBuffer();
	return qtrue;
}

/*
** mbReadPixelsAsync
**
** GL1 reads back with glReadPixels, which waits for the GPU. Here the copy is a blit in this frame's command
** buffer, and the callback runs once the frame has completed (MB_ProcessReadbacks).
*/
qboolean mbReadPixelsAsync( int x, int y, int width, int height, int format, mbReadbackFunc_t done, void *arg )
{
	mbReadback_t *r;
	NSUInteger bpp = format == MB_STENCIL_INDEX ? 1 : 4;

	if ( ( format != MB_RGB && format != MB_RGBA && format != MB_STENCIL_INDEX ) || width <= 0 || height <= 0
		|| x < 0 || y < 0 || x + width > mb_width || y + height > mb_height ) {
		return qfalse;
	}
	if ( mb_numReadbacks == MB_MAX_READBACKS ) {
		ri.Printf( PRINT_WARNING, "Metal: too many readbacks in flight\n" );
		return qfalse;
	}
	if ( !MB_ReadbackStart( format ) ) {
		return qfalse;
	}

	@autoreleasepool {
		id<MTLBuffer> buffer = [mb_device newBufferWithLength:(NSUInteger)width * height * bpp
			options:MTLResourceStorageModeShared];
		id<MTLBlitCommandEncoder> blit;

		if ( !buffer ) {
			return qfalse;
		}
		buffer.label = @"readback";

		// GL rows are bottom-up, Metal rows top-down (flipped when the pixels are handed out)
		blit = [mb_cmd blitCommandEncoder];
		[blit copyFromTexture:format == MB_STENCIL_INDEX ? mb_sceneDepth : mb_sceneColor sourceSlice:0 sourceLevel:0
			sourceOrigin:MTLOriginMake( x, mb_height - y - height, 0 ) sourceSize:MTLSizeMake( width, height, 1 )
			toBuffer:buffer destinationOffset:0 destinationBytesPerRow:width * bpp destinationBytesPerImage:width * height * bpp
			options:format == MB_STENCIL_INDEX ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionNone];
		[blit endEncoding];

		r = &mb_readbacks[mb_numReadbacks++];
		r->buffer = (__bridge_retained void *)buffer;
	}
	r->serial = mb_frameSerial;
	r->width = width;
	r->height = height;
	r->format = format;
	r->done = done;
	r->arg = arg;
	return qtrue;
}

/*
** MB_DeliverReadback
**
** The pixels as GL returns them (rows bottom row first, tightly packed), then the callback.
*/
static void MB_DeliverReadback( mbReadback_t *r )
{
	id<MTLBuffer> buffer = (__bridge_transfer id<MTLBuffer>)r->buffer;
	int srcBpp = r->format == MB_STENCIL_INDEX ? 1 : 4;
	int dstBpp = r->format == MB_RGB ? 3 : srcBpp;
	size_t size = (size_t)r->width * r->height * dstBpp;
	const byte *src;
	byte *dst;
	int row, col;

	r->buffer = NULL;
	if ( size > mb_readbackPixelsSize ) {
		free( mb_readbackPixels );
		mb_readbackPixels = malloc( size );
		mb_readbackPixelsSize = mb_readbackPixels ? size : 0;
		if ( !mb_readbackPixels ) {
			return;
		}
	}

	dst = mb_readbackPixels;
	for ( row = 0; row < r->height; row++ ) {
		src = (const byte *)buffer.contents + (size_t)( r->height - 1 - row ) * r->width * srcBpp;
		if ( dstBpp == srcBpp ) {
			memcpy( dst, src, (size_t)r->width * srcBpp );
			dst += (size_t)r->width * srcBpp;
			continue;
		}
		for ( col = 0; col < r->width; col++, src += 4, dst += 3 ) {
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
		}
	}

	r->done( mb_readbackPixels, r->width, r->height, r->arg );
}

/*
** MB_ProcessReadbacks
**
** Hands the readbacks of completed frames to their callbacks, oldest first.
*/
static void MB_ProcessReadbacks( uint64_t completed )
{
	mbReadback_t ready[MB_MAX_READBACKS];
	int i, kept, numReady;

	for ( i = kept = numReady = 0; i < mb_numReadbacks; i++ ) {
		if ( mb_readbacks[i].serial <= completed ) {
			ready[numReady++] = mb_readbacks[i];
		} else {
			mb_readbacks[kept++] = mb_readbacks[i];
		}
	}
	mb_numReadbacks = kept;

	// after the list is updated: a callback may start a readback of its own
	for ( i = 0; i < numReady; i++ ) {
		MB_DeliverReadback( &ready[i] );
	}
}

/*
** mbFlushReadbacks
**
** Submits what this frame recorded so far, waits for it and for the frames in flight, and runs the callbacks
** of every pending readback (vid_restart, map changes, quit, and synchronous reads).
*/
void mbFlushReadbacks( void )
{
	if ( !mb_numReadbacks ) {
		return;
	}
	if ( mb_frameSerial > 1 && mb_frameEvent.signaledValue < mb_frameSerial - 1 ) {
		[mb_frameEvent waitUntilSignaledValue:mb_frameSerial - 1 timeoutMS:2000];
	}
	MB_FlushAndWait();
	MB_ProcessReadbacks( UINT64_MAX );
}

typedef struct {
	byte	*pixels;
	int		bpp;
} mbReadCopy_t;

static void MB_CopyReadback( byte *pixels, int width, int height, void *arg )
{
	mbReadCopy_t *copy = arg;

	memcpy( copy->pixels, pixels, (size_t)width * height * copy->bpp );
}

/*
** mbReadPixels
**
** A synchronous read (video frames only, never in the normal frame path): the async readback, then a wait.
*/
void mbReadPixels( int x, int y, int width, int height, int format, int type, void *pixels )
{
	mbReadCopy_t copy;

	copy.pixels = pixels;
	copy.bpp = format == MB_RGBA ? 4 : ( format == MB_RGB ? 3 : 1 );
	if ( type == MB_FLOAT ) {
		copy.bpp *= sizeof( float );
	}

	if ( type != MB_UNSIGNED_BYTE || ( format != MB_RGB && format != MB_RGBA )
		|| !mbReadPixelsAsync( x, y, width, height, format, MB_CopyReadback, &copy ) ) {
		memset( pixels, 0, (size_t)width * height * copy.bpp );
		return;
	}
	mbFlushReadbacks();
}

/*
** MB_HarvestQueries
**
** Copies the results of completed frames out of their slots' visibility buffers and zeroes the counters used
** (the buffers accumulate).
*/
static void MB_HarvestQueries( uint64_t completed )
{
	int slot, i;

	for ( slot = 0; slot < MB_MAX_FRAMES; slot++ ) {
		uint64_t serial = mb_querySerial[slot];
		mbQueryResults_t *r;
		uint64_t *counts;

		if ( !serial || serial > completed ) {
			continue;
		}
		r = &mb_queryResults[serial % MB_QUERY_HISTORY];
		counts = mb_queryBuffers[slot].contents;
		r->serial = serial;
		r->count = mb_queryCount[slot];
		for ( i = 0; i < r->count; i++ ) {
			r->passed[i] = counts[i] != 0;
		}
		memset( counts, 0, (size_t)r->count * sizeof( *counts ) );
		mb_querySerial[slot] = 0;
		mb_queryCount[slot] = 0;
	}
}

/*
** mbDepthQuery
**
** GL1 reads the depth under each flare with glReadPixels (a full GPU sync per flare). Here a point at the
** pixel's center is depth-tested in the scene pass, counted by the visibility result buffer.
*/
int mbDepthQuery( int x, int y, float depth )
{
	int slot = mb_frameSlot;
	int index;
	float position[4];

	if ( !mb_queryPipeline || !mb_sceneColor || x < 0 || y < 0 || x >= mb_width || y >= mb_height ) {
		return 0;
	}

	if ( mb_querySerial[slot] != mb_frameSerial ) {
		// the slot was harvested at the end of the frame that used it last
		mb_querySerial[slot] = mb_frameSerial;
		mb_queryCount[slot] = 0;
	}
	index = mb_queryCount[slot];
	if ( index == MB_MAX_QUERIES ) {
		return 0;
	}
	if ( !mb_queryBuffers[slot] ) {
		mb_queryBuffers[slot] = [mb_device newBufferWithLength:MB_MAX_QUERIES * sizeof( uint64_t ) options:MTLResourceStorageModeShared];
		if ( !mb_queryBuffers[slot] ) {
			return 0;
		}
		mb_queryBuffers[slot].label = @"depth queries";
		memset( mb_queryBuffers[slot].contents, 0, MB_MAX_QUERIES * sizeof( uint64_t ) );
	}

	// the pass needs the visibility buffer: a pass begun without it is ended (once; later passes get it)
	mb_queryKeep = MB_QUERY_KEEP;
	if ( mb_enc && !mb_encQueries ) {
		MB_EndPass( qtrue );
	}
	if ( !MB_BeginPass() || !mb_encQueries ) {
		return 0;
	}

	// GL window coordinates (origin bottom-left) to Metal's normalized device coordinates
	position[0] = ( x + 0.5f ) * 2.0f / mb_width - 1.0f;
	position[1] = ( y + 0.5f ) * 2.0f / mb_height - 1.0f;
	position[2] = depth < 0.0f ? 0.0f : ( depth > 1.0f ? 1.0f : depth );
	position[2] = mb_depthFloor + ( 1.0f - mb_depthFloor ) * position[2];	// the scene's full range (mbDepthFloor)
	position[3] = 1.0f;

	[mb_enc setRenderPipelineState:mb_queryPipeline];
	[mb_enc setDepthStencilState:MB_OBJ( MB_DepthStencilState( 1 | ( (uint64_t)MTLCompareFunctionLess << 1 ) ) )];
	[mb_enc setViewport:(MTLViewport){ 0.0, 0.0, mb_width, mb_height, 0.0, 1.0 }];
	[mb_enc setScissorRect:(MTLScissorRect){ 0, 0, (NSUInteger)mb_width, (NSUInteger)mb_height }];
	[mb_enc setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
	[mb_enc setVertexBytes:position length:sizeof( position ) atIndex:1];
	[mb_enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)index * sizeof( uint64_t )];
	[mb_enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
	[mb_enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
	mb_statDraws++;

	// the draw state must be set again
	mb_encPipelineKey = 0xffffffff;
	mb_encDepthStencil = NULL;
	mb_dirty |= MB_DIRTY_VIEWPORT | MB_DIRTY_SCISSOR | MB_DIRTY_RASTER | MB_DIRTY_DEPTHSTENCIL | MB_DIRTY_PIPELINE;

	mb_queryCount[slot]++;
	return ( MB_QUERY_TAG( mb_frameSerial ) << 8 ) | index;
}

/*
** mbDepthQueryResult
*/
int mbDepthQueryResult( int handle )
{
	int tag = (int)( (unsigned int)handle >> 8 );
	int index = handle & 255;
	int i;

	if ( !tag ) {
		return -2;
	}
	for ( i = 0; i < MB_QUERY_HISTORY; i++ ) {
		const mbQueryResults_t *r = &mb_queryResults[i];

		if ( r->serial && MB_QUERY_TAG( r->serial ) == tag ) {
			return index < r->count ? r->passed[index] : -2;
		}
	}
	for ( i = 0; i < MB_MAX_FRAMES; i++ ) {
		if ( mb_querySerial[i] && MB_QUERY_TAG( mb_querySerial[i] ) == tag ) {
			return -1;		// still in flight
		}
	}
	return -2;
}

// RE_DrawBackground_DrawPixels only, and GL1 leaves the DrawBackground export NULL
void mbDrawPixels( int width, int height, int format, int type, const void *pixels ) {}
void mbPixelZoom( float xfactor, float yfactor ) {}
