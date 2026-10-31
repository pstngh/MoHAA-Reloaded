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

// vk_framelog.c -- per-frame log, to find what makes the slowest frames
//
// With r_vkFrameLog set when the renderer starts (latched: +set on the
// command line, or set it and vid_restart), the exports the engine calls
// every frame are wrapped (the same ones as tools/rtimer/rtimer.c, so the
// numbers mean the same for GL1 measured with it), and every frame is
// recorded in a ring of the last r_vkFrameLog frames (1: 131072): the main
// thread's time in the renderer's exports and between them, what the
// backend did (the vkStats_t counters of that frame: waits, submissions,
// acquires, presents, pipelines, textures and memory created, flushes, ...)
// and the GPU time of the frame from its timestamps; with r_vkSubmitThread
// also the submit thread's times for the frame. `vkframelog [file]`
// writes the frames recorded since the last `vkframelog` to
// <fs_homepath>/main/<file> (default vkframes.txt) and prints a summary;
// `vkframelog clear` drops them. With r_vkFrameLog 0 nothing is wrapped and
// the backend only tests vk_frameLog once per frame.
//
// A frame goes from one BeginFrame to the next. Its time splits into the
// exports (begin, scene: RenderScene, the front end; 2d: the 2D exports, in
// which OpenMoHAA runs the queued render commands, so the 3D view's back
// end and the fence wait at the start of the frame are there; end:
// EndFrame, the submission, present and the acquire of the next image;
// load: the registration exports), cgame (from BeginFrame to EndFrame
// outside of the exports: the UI and cgame, which builds the scene) and
// engine (from EndFrame's return to the next BeginFrame: sound mixing, the
// frame limiter's sleep, input and network events, the local server's game
// frame, the client's packets).

#include "tr_local.h"
#include "vk_local.h"

#define VK_FRAMELOG_DEFAULT		131072
#define VK_FRAMELOG_MIN			1024
#define VK_FRAMELOG_WORST		12		// frames listed in the summary

qboolean	vk_frameLog;

// one frame; times in microseconds
typedef struct {
	uint32_t	serial;			// vk.frameSerial of the frame (low bits)
	uint32_t	start;			// BeginFrame, since the log started
	uint32_t	interval;		// to the next BeginFrame
	uint32_t	presentAt;		// vkQueuePresentKHR, since the log started; 0: no present
	// main thread
	uint32_t	begin, scene, twoD, end, load;
	uint32_t	engine;			// from EndFrame's return to the next BeginFrame
	// backend (vkStats_t of the frame)
	uint32_t	fenceWait;		// frame fence
	uint32_t	flushWait;		// mid-frame flushes
	uint32_t	submit, acquire, acquireAhead, present;
	uint32_t	pipelineUs, textureUs, allocationUs;
	uint32_t	stagingBytes, vertexBytes, vertexGrowth, skinBytes;
	uint32_t	draws;
	uint16_t	pipelines, textures, allocations, flushes, skinUploads;
	uint16_t	submits, renderPasses, depthTargets, swapchains;
	// GPU, from the timestamps (set when the frame slot is reused)
	uint32_t	gpuMain;		// main command buffer
	uint32_t	gpuStart, gpuEnd;	// since the first timestamp of the log; gpuEnd 0: none
	// r_vkSubmitThread: the main thread's waits for the submit thread in
	// the frame, and the thread's times for the frame (set when the main
	// thread joins it): from the hand-off to its start, in vkQueueSubmit,
	// vkAcquireNextImageKHR and vkQueuePresentKHR
	uint32_t	threadWait;
	uint32_t	threadStart, threadSubmit, threadAcquire, threadPresent;
} vkFrameRecord_t;

static struct {
	refexport_t		real;
	refexport_t		exports;
	cvar_t			*cvar;

	vkFrameRecord_t	*records;
	int				size;
	uint64_t		count;			// records stored
	uint64_t		first;			// first record of the current window
	uint64_t		lost;			// records of the window overwritten before a dump

	uint64_t		base;			// VK_Microseconds when the log started
	qboolean		inFrame;		// cur holds a frame begun
	int				depth;			// wrapped exports being run (see FL_TIME)
	vkFrameRecord_t	cur;
	uint64_t		curStart;		// VK_Microseconds of cur's BeginFrame
	uint64_t		lastEndReturn;
	uint64_t		exportsAtEnd;	// cur's time in the exports at EndFrame's return
	vkStats_t		stats;			// vk.stats at the last EndFrame
	uint64_t		lastPresent;	// vk.lastPresentUs at the last EndFrame
	qboolean		haveGpuBase;
	uint64_t		gpuBase;		// first timestamp, in ticks
} fl;

static uint32_t FL_Clamp( uint64_t v ) {
	return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v;
}

static uint16_t FL_Clamp16( uint64_t v ) {
	return v > 0xFFFFull ? 0xFFFFu : (uint16_t)v;
}

static uint64_t FL_Exports( const vkFrameRecord_t *r ) {
	return (uint64_t)r->begin + r->scene + r->twoD + r->end + r->load;
}

/*
===============
VK_FrameLogGpu

The timestamps of a frame, when its slot is reused (two frames later):
ticks of the main command buffer's start and end and of the frame's end
===============
*/
void VK_FrameLogGpu( uint64_t serial, uint64_t mainStart, uint64_t mainEnd, uint64_t frameEnd, uint64_t mask, double usPerTick ) {
	uint64_t i;

	if ( !fl.records ) {
		return;
	}
	if ( !fl.haveGpuBase ) {
		fl.gpuBase = mainStart;
		fl.haveGpuBase = qtrue;
	}
	// the frame is one of the last few stored
	for ( i = fl.count; i > 0 && i + 4 > fl.count; i-- ) {
		vkFrameRecord_t *r = &fl.records[( i - 1 ) % fl.size];

		if ( r->serial == (uint32_t)serial ) {
			r->gpuMain = FL_Clamp( ( ( mainEnd - mainStart ) & mask ) * usPerTick );
			r->gpuStart = FL_Clamp( ( ( mainStart - fl.gpuBase ) & mask ) * usPerTick );
			r->gpuEnd = FL_Clamp( ( ( frameEnd - fl.gpuBase ) & mask ) * usPerTick );
			if ( !r->gpuEnd ) {
				r->gpuEnd = 1;
			}
			return;
		}
	}
}

// the record of a frame among the last few: the frame being recorded (its
// serial is set when its EndFrame returns; with FIFO the submit thread's
// frame is joined inside it) or one stored
static vkFrameRecord_t *FL_FindRecord( uint64_t serial ) {
	uint64_t i;

	if ( fl.inFrame && ( fl.cur.serial == (uint32_t)serial || ( !fl.cur.serial && serial == vk.frameSerial ) ) ) {
		return &fl.cur;
	}
	for ( i = fl.count; i > 0 && i + 8 > fl.count; i-- ) {
		vkFrameRecord_t *r = &fl.records[( i - 1 ) % fl.size];

		if ( r->serial == (uint32_t)serial ) {
			return r;
		}
	}
	return NULL;
}

/*
===============
VK_FrameLogThread

The submit thread's times for a frame (vk_submit.c), when the main thread
joins it: its present, and the times from the hand-off to its start and in
its calls
===============
*/
void VK_FrameLogThread( uint64_t serial, uint64_t presentAt, uint64_t startUs, uint64_t submitUs, uint64_t acquireUs, uint64_t presentUs ) {
	vkFrameRecord_t *r;

	if ( !fl.records || !( r = FL_FindRecord( serial ) ) ) {
		return;
	}
	if ( presentAt > fl.base ) {
		r->presentAt = FL_Clamp( presentAt - fl.base );
	}
	r->threadStart = FL_Clamp( startUs );
	r->threadSubmit = FL_Clamp( submitUs );
	r->threadAcquire = FL_Clamp( acquireUs );
	r->threadPresent = FL_Clamp( presentUs );
}

/*
====================================================================

WRAPPED EXPORTS

====================================================================
*/

static void FL_BeginFrame( stereoFrame_t stereoFrame ) {
	uint64_t t0 = VK_Microseconds();

	// the frame that ends here; registration between frames (map loads)
	// isn't engine time
	if ( fl.inFrame ) {
		uint64_t between = t0 - fl.lastEndReturn, exports = FL_Exports( &fl.cur ) - fl.exportsAtEnd;

		fl.cur.interval = FL_Clamp( t0 - fl.curStart );
		fl.cur.engine = FL_Clamp( between > exports ? between - exports : 0 );
		if ( fl.count >= fl.first && fl.count - fl.first >= (uint64_t)fl.size ) {
			fl.lost++;
		}
		fl.records[fl.count % fl.size] = fl.cur;
		fl.count++;
	}

	Com_Memset( &fl.cur, 0, sizeof( fl.cur ) );
	fl.curStart = t0;
	fl.cur.start = FL_Clamp( t0 - fl.base );
	fl.inFrame = qtrue;
	// what the backend did between frames (a map load: its pipelines,
	// textures and uploads) is no frame's, like the registration time
	fl.stats = vk.stats;

	// not nested in anything; an error that jumped out of an export left
	// depth behind
	fl.depth = 1;
	fl.real.BeginFrame( stereoFrame );
	fl.depth = 0;
	fl.cur.begin += FL_Clamp( VK_Microseconds() - t0 );
}

// times call into cur.field unless it runs inside another wrapped export:
// the registration exports call the engine's TIKI code, which registers
// shaders through the exports again, and nested time would count twice
#define FL_TIME( field, call ) { \
	if ( fl.depth++ ) { \
		call; \
	} else { \
		uint64_t t0 = VK_Microseconds(); \
		call; \
		fl.cur.field += FL_Clamp( VK_Microseconds() - t0 ); \
	} \
	fl.depth--; \
}
#define FL_TIME_RETURN( type, field, call ) { type r; FL_TIME( field, r = call ) return r; }

static void FL_RenderScene( const refdef_t *fd ) {
	FL_TIME( scene, fl.real.RenderScene( fd ) )
}

static void FL_EndFrame( int *frontEndMsec, int *backEndMsec ) {
	const vkStats_t *s = &vk.stats, *p = &fl.stats;
	uint64_t t0 = VK_Microseconds(), t1;
	vkFrameRecord_t *r = &fl.cur;

	fl.depth++;
	fl.real.EndFrame( frontEndMsec, backEndMsec );
	fl.depth--;
	t1 = VK_Microseconds();
	r->end += FL_Clamp( t1 - t0 );
	fl.lastEndReturn = t1;
	fl.exportsAtEnd = FL_Exports( r );

	// what the backend did since the last EndFrame (the serial of the frame
	// is set as it ends, before the submit thread's times can come)
	r->serial = (uint32_t)vk.frameSerial;
	if ( vk.lastPresentUs != fl.lastPresent && vk.lastPresentUs > fl.base ) {
		r->presentAt = FL_Clamp( vk.lastPresentUs - fl.base );
	}
	fl.lastPresent = vk.lastPresentUs;
	r->fenceWait = FL_Clamp( ( s->waitUs - p->waitUs ) - ( s->flushUs - p->flushUs ) );
	r->flushWait = FL_Clamp( s->flushUs - p->flushUs );
	r->submit = FL_Clamp( s->submitUs - p->submitUs );
	r->acquire = FL_Clamp( s->acquireUs - p->acquireUs );
	r->acquireAhead = FL_Clamp( s->acquireAheadUs - p->acquireAheadUs );
	r->present = FL_Clamp( s->presentUs - p->presentUs );
	r->pipelineUs = FL_Clamp( s->pipelineUs - p->pipelineUs );
	r->textureUs = FL_Clamp( s->textureUs - p->textureUs );
	r->allocationUs = FL_Clamp( s->allocationUs - p->allocationUs );
	r->stagingBytes = FL_Clamp( s->stagingBytes - p->stagingBytes );
	r->vertexBytes = FL_Clamp( s->vertexBytes - p->vertexBytes );
	r->vertexGrowth = FL_Clamp( s->vertexGrowth - p->vertexGrowth );
	r->draws = FL_Clamp( s->draws - p->draws );
	r->skinBytes = FL_Clamp( s->skinBytes - p->skinBytes );
	r->pipelines = FL_Clamp16( s->pipelines - p->pipelines );
	r->skinUploads = FL_Clamp16( s->skinUploads - p->skinUploads );
	r->textures = FL_Clamp16( s->textures - p->textures );
	r->allocations = FL_Clamp16( s->allocations - p->allocations );
	r->flushes = FL_Clamp16( s->flushes - p->flushes );
	r->submits = FL_Clamp16( s->submits - p->submits );
	r->renderPasses = FL_Clamp16( s->renderPasses - p->renderPasses );
	r->depthTargets = FL_Clamp16( s->depthTargets - p->depthTargets );
	r->swapchains = FL_Clamp16( s->swapchains - p->swapchains );
	r->threadWait = FL_Clamp( s->threadWaitUs - p->threadWaitUs );
	fl.stats = vk.stats;
}

// the 2D exports: time added to cur.twoD
#define FL_2D( call ) FL_TIME( twoD, call )
static void FL_Set2DWindow( int x, int y, int w, int h, float left, float right, float bottom, float top, float n, float f ) {
	FL_2D( fl.real.Set2DWindow( x, y, w, h, left, right, bottom, top, n, f ) )
}
static void FL_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	FL_2D( fl.real.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, hShader ) )
}
static void FL_DrawStretchPic2( float x, float y, float w, float h, float s1, float t1, float s2, float t2, float sx, float sy, qhandle_t hShader ) {
	FL_2D( fl.real.DrawStretchPic2( x, y, w, h, s1, t1, s2, t2, sx, sy, hShader ) )
}
static void FL_DrawTilePic( float x, float y, float w, float h, qhandle_t hShader ) {
	FL_2D( fl.real.DrawTilePic( x, y, w, h, hShader ) )
}
static void FL_DrawTilePicOffset( float x, float y, float w, float h, qhandle_t hShader, int offsetX, int offsetY ) {
	FL_2D( fl.real.DrawTilePicOffset( x, y, w, h, hShader, offsetX, offsetY ) )
}
static void FL_DrawTrianglePic( const vec2_t vPoints[3], const vec2_t vTexCoords[3], qhandle_t hShader ) {
	FL_2D( fl.real.DrawTrianglePic( vPoints, vTexCoords, hShader ) )
}
static void FL_DrawBackground( int cols, int rows, int bgr, uint8_t *data ) {
	FL_2D( fl.real.DrawBackground( cols, rows, bgr, data ) )
}
static void FL_DrawBox( float x, float y, float w, float h ) {
	FL_2D( fl.real.DrawBox( x, y, w, h ) )
}
static void FL_AddBox( float x, float y, float w, float h ) {
	FL_2D( fl.real.AddBox( x, y, w, h ) )
}
static void FL_DrawBoxes( const float (*boxes)[4], int count, const float *color ) {
	FL_2D( fl.real.DrawBoxes( boxes, count, color ) )
}
static void FL_Scissor( int x, int y, int width, int height ) {
	FL_2D( fl.real.Scissor( x, y, width, height ) )
}
static void FL_DrawLineLoop( const vec2_t *points, int count, int stippleFactor, int stippleMask ) {
	FL_2D( fl.real.DrawLineLoop( points, count, stippleFactor, stippleMask ) )
}
static void FL_DrawString( fontheader_t *font, const char *text, float x, float y, int maxLen, const float *pvVirtualScreen ) {
	FL_2D( fl.real.DrawString( font, text, x, y, maxLen, pvVirtualScreen ) )
}
static void FL_DrawStretchRaw( int x, int y, int w, int h, int cols, int rows, int components, const byte *data ) {
	FL_2D( fl.real.DrawStretchRaw( x, y, w, h, cols, rows, components, data ) )
}

// the registration exports: time added to cur.load
#define FL_LOAD( type, call ) FL_TIME_RETURN( type, load, call )
#define FL_LOAD_VOID( call ) FL_TIME( load, call )
static void FL_BeginRegistration( glconfig_t *glconf ) {
	FL_LOAD_VOID( fl.real.BeginRegistration( glconf ) )
}
static void FL_LoadWorld( const char *name ) {
	FL_LOAD_VOID( fl.real.LoadWorld( name ) )
}
static void FL_EndRegistration( void ) {
	FL_LOAD_VOID( fl.real.EndRegistration() )
}
static qhandle_t FL_RegisterModel( const char *name ) {
	FL_LOAD( qhandle_t, fl.real.RegisterModel( name ) )
}
static qhandle_t FL_RegisterSkin( const char *name ) {
	FL_LOAD( qhandle_t, fl.real.RegisterSkin( name ) )
}
static qhandle_t FL_RegisterShader( const char *name ) {
	FL_LOAD( qhandle_t, fl.real.RegisterShader( name ) )
}
static qhandle_t FL_RegisterShaderNoMip( const char *name ) {
	FL_LOAD( qhandle_t, fl.real.RegisterShaderNoMip( name ) )
}
static qhandle_t FL_RegisterServerModel( const char *name ) {
	FL_LOAD( qhandle_t, fl.real.RegisterServerModel( name ) )
}
static qhandle_t FL_RefreshShaderNoMip( const char *name ) {
	FL_LOAD( qhandle_t, fl.real.RefreshShaderNoMip( name ) )
}
static void FL_RegisterFont( const char *fontName, int pointSize, fontInfo_t *font ) {
	FL_LOAD_VOID( fl.real.RegisterFont( fontName, pointSize, font ) )
}
static fontheader_t *FL_LoadFont( const char *name ) {
	FL_LOAD( fontheader_t *, fl.real.LoadFont( name ) )
}

/*
====================================================================

DUMP

====================================================================
*/

typedef struct {
	fileHandle_t	f;
	char			buf[65536];
	int				used;
} flWriter_t;

static void FL_Flush( flWriter_t *w ) {
	if ( w->used ) {
		ri.FS_Write( w->buf, w->used, w->f );
		w->used = 0;
	}
}

static Q_PRINTF_FUNC( 2, 3 ) void FL_Printf( flWriter_t *w, const char *fmt, ... ) {
	va_list ap;
	int n;

	if ( w->used > (int)sizeof( w->buf ) - 1024 ) {
		FL_Flush( w );
	}
	va_start( ap, fmt );
	n = Q_vsnprintf( w->buf + w->used, sizeof( w->buf ) - w->used, fmt, ap );
	va_end( ap );
	if ( n > 0 ) {
		w->used += Q_min( n, (int)sizeof( w->buf ) - w->used - 1 );
	}
}

static int FL_CompareUint( const void *a, const void *b ) {
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

// value at fraction q of the sorted values (q = 0.99: the 99th percentile)
static uint32_t FL_Percentile( const uint32_t *sorted, int n, double q ) {
	int i = (int)( n * q );

	return sorted[Q_min( i, n - 1 )];
}

static uint32_t FL_Cgame( const vkFrameRecord_t *r ) {
	uint64_t inside = FL_Exports( r ) + r->engine;

	return inside < r->interval ? r->interval - (uint32_t)inside : 0;
}

/*
===============
FL_Distribution

Mean, median, percentiles and maximum of n values (sorted in place), in ms
===============
*/
static void FL_Distribution( uint32_t *v, int n, char *out, int outSize ) {
	double sum = 0;
	int i;

	for ( i = 0; i < n; i++ ) {
		sum += v[i];
	}
	qsort( v, n, sizeof( v[0] ), FL_CompareUint );
	Com_sprintf( out, outSize, "mean %.3f (%.0f fps), median %.3f, p90 %.3f, p99 %.3f (%.0f fps), p99.9 %.3f (%.0f fps), max %.3f",
		sum / n / 1000.0, n * 1e6 / Q_max( sum, 1 ),
		FL_Percentile( v, n, 0.5 ) / 1000.0, FL_Percentile( v, n, 0.9 ) / 1000.0,
		FL_Percentile( v, n, 0.99 ) / 1000.0, 1e6 / Q_max( FL_Percentile( v, n, 0.99 ), 1 ),
		FL_Percentile( v, n, 0.999 ) / 1000.0, 1e6 / Q_max( FL_Percentile( v, n, 0.999 ), 1 ),
		v[n - 1] / 1000.0 );
}

// the events of a frame, for the summary
static void FL_Events( const vkFrameRecord_t *r, char *out, int outSize ) {
	*out = 0;
	if ( r->pipelines ) {
		Q_strcat( out, outSize, va( " pipelines %d (%.2f ms)", r->pipelines, r->pipelineUs / 1000.0 ) );
	}
	if ( r->textures ) {
		Q_strcat( out, outSize, va( " textures %d (%.2f ms)", r->textures, r->textureUs / 1000.0 ) );
	}
	if ( r->stagingBytes ) {
		Q_strcat( out, outSize, va( " uploads %.0f KB", r->stagingBytes / 1024.0 ) );
	}
	if ( r->skinUploads ) {
		Q_strcat( out, outSize, va( " skin data %d (%.0f KB)", r->skinUploads, r->skinBytes / 1024.0 ) );
	}
	if ( r->allocations ) {
		Q_strcat( out, outSize, va( " allocations %d (%.2f ms)", r->allocations, r->allocationUs / 1000.0 ) );
	}
	if ( r->vertexGrowth ) {
		Q_strcat( out, outSize, va( " vertex buffer grown to %.0f KB", r->vertexGrowth / 1024.0 ) );
	}
	if ( r->flushes ) {
		Q_strcat( out, outSize, va( " flushes %d (%.2f ms)", r->flushes, r->flushWait / 1000.0 ) );
	}
	if ( r->depthTargets ) {
		Q_strcat( out, outSize, " depth image replaced" );
	}
	if ( r->swapchains ) {
		Q_strcat( out, outSize, " swapchain created" );
	}
	if ( r->load >= 50 ) {
		Q_strcat( out, outSize, va( " registration %.2f ms", r->load / 1000.0 ) );
	}
}

static const vkFrameRecord_t *FL_Record( uint64_t i ) {
	return &fl.records[i % fl.size];
}

static void FL_Dump( const char *name ) {
	uint64_t first = fl.first;
	int n, np, i, k;
	int worst[VK_FRAMELOG_WORST];
	uint32_t *v;
	flWriter_t *w;
	char dist[512], line[1024];

	if ( first + fl.size < fl.count ) {
		first = fl.count - fl.size;
	}
	n = first < fl.count ? (int)( fl.count - first ) : 0;
	if ( n < 2 ) {
		ri.Printf( PRINT_ALL, "vkframelog: no frames recorded\n" );
		return;
	}

	w = malloc( sizeof( *w ) );
	v = malloc( sizeof( v[0] ) * n );
	if ( !w || !v ) {
		free( w );
		free( v );
		ri.Printf( PRINT_ALL, "vkframelog: out of memory\n" );
		return;
	}
	w->used = 0;
	w->f = ri.FS_OpenFileWrite( name );
	if ( !w->f ) {
		ri.Printf( PRINT_ALL, "vkframelog: can't write %s\n", name );
		free( w );
		free( v );
		return;
	}

	FL_Printf( w, "# vkframelog 1: renderer_vulkan frame log (see code/renderervk/README.md, Performance)\n" );
	FL_Printf( w, "# device %s, %s, present mode %s, %dx%d, %d swapchain images, r_swapInterval %d, com_maxfps %d, r_ext_multisample %d,"
		" r_vkSubmitThread %d, r_smp %d, %d frames in flight\n",
		glConfig.renderer_string, glConfig.version_string, VK_SwapchainPresentModeName(), vk.width, vk.height,
		vk.swapchainImageCount, ri.Cvar_VariableIntegerValue( "r_swapInterval" ), ri.Cvar_VariableIntegerValue( "com_maxfps" ),
		ri.Cvar_VariableIntegerValue( "r_ext_multisample" ), vk.submitThread, glConfig.smpActive, VK_MAX_FRAMES );
	FL_Printf( w, "# %d frames%s, %.1f s\n", n,
		fl.lost ? va( " (%llu older ones overwritten: raise r_vkFrameLog)", (unsigned long long)fl.lost ) : "",
		( FL_Record( fl.count - 1 )->start - FL_Record( first )->start ) / 1e6 );

	// frame time: BeginFrame to BeginFrame
	for ( i = 0; i < n; i++ ) {
		v[i] = FL_Record( first + i )->interval;
	}
	FL_Distribution( v, n, dist, sizeof( dist ) );
	FL_Printf( w, "# frame time ms: %s\n", dist );
	ri.Printf( PRINT_ALL, "vkframelog: %d frames, frame time ms: %s\n", n, dist );
	// present to present
	for ( i = 1, np = 0; i < n; i++ ) {
		const vkFrameRecord_t *a = FL_Record( first + i - 1 ), *b = FL_Record( first + i );

		if ( a->presentAt && b->presentAt ) {
			v[np++] = b->presentAt - a->presentAt;
		}
	}
	if ( np > 1 ) {
		FL_Distribution( v, np, dist, sizeof( dist ) );
		FL_Printf( w, "# present to present ms: %s\n", dist );
		ri.Printf( PRINT_ALL, "vkframelog: present to present ms: %s\n", dist );
	}

	// the slowest frames
	for ( k = 0; k < VK_FRAMELOG_WORST; k++ ) {
		worst[k] = -1;
		for ( i = 0; i < n; i++ ) {
			int j;

			for ( j = 0; j < k && worst[j] != i; j++ ) {
			}
			if ( j == k && ( worst[k] < 0 || FL_Record( first + i )->interval > FL_Record( first + worst[k] )->interval ) ) {
				worst[k] = i;
			}
		}
	}
	FL_Printf( w, "# slowest frames (ms):\n" );
	ri.Printf( PRINT_ALL, "vkframelog: slowest frames (ms):\n" );
	for ( k = 0; k < VK_FRAMELOG_WORST && k < n; k++ ) {
		const vkFrameRecord_t *r = FL_Record( first + worst[k] );
		char events[512];

		FL_Events( r, events, sizeof( events ) );
		Com_sprintf( line, sizeof( line ), "frame %d: %.3f = scene %.3f, 2d %.3f (fence %.3f), end %.3f (submit %.3f, present %.3f, acquire %.3f%s),"
			" cgame %.3f, engine %.3f; gpu %.3f%s%s",
			worst[k], r->interval / 1000.0, r->scene / 1000.0, r->twoD / 1000.0, r->fenceWait / 1000.0, r->end / 1000.0,
			r->submit / 1000.0, r->present / 1000.0, ( r->acquire + r->acquireAhead ) / 1000.0,
			r->threadWait ? va( ", submit thread %.3f", r->threadWait / 1000.0 ) : "", FL_Cgame( r ) / 1000.0,
			r->engine / 1000.0, r->gpuEnd ? r->gpuMain / 1000.0 : 0.0,
			vk.submitThread ? va( "; submit thread: start %.3f, submit %.3f, acquire %.3f, present %.3f", r->threadStart / 1000.0,
				r->threadSubmit / 1000.0, r->threadAcquire / 1000.0, r->threadPresent / 1000.0 ) : "", events );
		FL_Printf( w, "#   %s\n", line );
		ri.Printf( PRINT_ALL, "  %s\n", line );
	}

	FL_Printf( w, "# columns, microseconds unless named: frame start interval begin scene 2d end load cgame engine present_at"
		" fence flush submit acquire acquire_ahead present gpu gpu_start gpu_end draws render_passes submits vertex_bytes staging_bytes"
		" pipelines pipeline_us textures texture_us allocations allocation_us flushes vertex_growth depth_targets swapchains"
		" skin_uploads skin_bytes thread_wait thread_start thread_submit thread_acquire thread_present\n" );
	FL_Printf( w, "# (frame: index in this log; start, present_at, gpu_start and gpu_end: since the log started, 0 and gpu_end 0: none;"
		" engine: EndFrame's return to the next BeginFrame; cgame: the rest of the interval outside the exports;"
		" thread_wait: the main thread waiting for the submit thread; thread_*: the submit thread's times for the frame,"
		" from the hand-off to its start and in its calls; submit, acquire, acquire_ahead, present: the main thread's)\n" );
	for ( i = 0; i < n; i++ ) {
		const vkFrameRecord_t *r = FL_Record( first + i );

		FL_Printf( w, "%d %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
			i, r->start, r->interval, r->begin, r->scene, r->twoD, r->end, r->load, FL_Cgame( r ), r->engine, r->presentAt,
			r->fenceWait, r->flushWait, r->submit, r->acquire, r->acquireAhead, r->present, r->gpuMain, r->gpuStart, r->gpuEnd,
			r->draws, r->renderPasses, r->submits, r->vertexBytes, r->stagingBytes,
			r->pipelines, r->pipelineUs, r->textures, r->textureUs, r->allocations, r->allocationUs, r->flushes,
			r->vertexGrowth, r->depthTargets, r->swapchains, r->skinUploads, r->skinBytes,
			r->threadWait, r->threadStart, r->threadSubmit, r->threadAcquire, r->threadPresent );
	}
	FL_Flush( w );
	ri.FS_CloseFile( w->f );
	ri.Printf( PRINT_ALL, "vkframelog: wrote %d frames to %s\n", n, name );
	free( w );
	free( v );
}

/*
===============
FL_Command_f

vkframelog [file]: writes the frames recorded since the last vkframelog
(at most the ring's) to file, default vkframes.txt; vkframelog clear: drops
them. The frame in progress, whose time includes the writing, is left out
of the next window too.
===============
*/
static void FL_Command_f( void ) {
	const char *arg = ri.Cmd_Argc() > 1 ? ri.Cmd_Argv( 1 ) : "vkframes.txt";

	if ( Q_stricmp( arg, "clear" ) ) {
		FL_Dump( arg );
	}
	fl.first = fl.count + 1;
	fl.lost = 0;
}

static void FL_Shutdown( qboolean destroyWindow ) {
	fl.real.Shutdown( destroyWindow );
	if ( destroyWindow ) {
		// the engine unloads the renderer
		ri.Cmd_RemoveCommand( "vkframelog" );
		free( fl.records );
		fl.records = NULL;
		vk_frameLog = qfalse;
	}
}

/*
===============
VK_FrameLogExports

Called by GetRefAPI with the renderer's exports: returns them, or the
wrapped ones when r_vkFrameLog is set
===============
*/
refexport_t *VK_FrameLogExports( refexport_t *real ) {
	refexport_t *e = &fl.exports;

	fl.cvar = ri.Cvar_Get( "r_vkFrameLog", "0", CVAR_LATCH );
	ri.Cvar_SetDescription( fl.cvar, "frames kept by the frame log (1: 131072, 0: off), see vkframelog" );
	if ( !fl.cvar->integer ) {
		return real;
	}

	free( fl.records );
	Com_Memset( &fl, 0, sizeof( fl ) );
	fl.cvar = ri.Cvar_Get( "r_vkFrameLog", "0", CVAR_LATCH );
	fl.size = fl.cvar->integer == 1 ? VK_FRAMELOG_DEFAULT : Q_max( fl.cvar->integer, VK_FRAMELOG_MIN );
	fl.records = malloc( sizeof( fl.records[0] ) * fl.size );
	if ( !fl.records ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: r_vkFrameLog: out of memory\n" );
		return real;
	}
	fl.base = VK_Microseconds();
	fl.stats = vk.stats;
	fl.lastPresent = vk.lastPresentUs;
	fl.lastEndReturn = fl.base;
	vk_frameLog = qtrue;

	fl.real = *real;
	*e = *real;
	e->Shutdown = FL_Shutdown;
	e->BeginFrame = FL_BeginFrame;
	e->RenderScene = FL_RenderScene;
	e->EndFrame = FL_EndFrame;
	e->Set2DWindow = FL_Set2DWindow;
	e->DrawStretchPic = FL_DrawStretchPic;
	e->DrawStretchPic2 = FL_DrawStretchPic2;
	e->DrawTilePic = FL_DrawTilePic;
	e->DrawTilePicOffset = FL_DrawTilePicOffset;
	e->DrawTrianglePic = FL_DrawTrianglePic;
	e->DrawBackground = FL_DrawBackground;
	e->DrawBox = FL_DrawBox;
	e->AddBox = FL_AddBox;
	e->DrawBoxes = FL_DrawBoxes;
	e->Scissor = FL_Scissor;
	e->DrawLineLoop = FL_DrawLineLoop;
	e->DrawString = FL_DrawString;
	e->DrawStretchRaw = FL_DrawStretchRaw;
	e->BeginRegistration = FL_BeginRegistration;
	e->LoadWorld = FL_LoadWorld;
	e->EndRegistration = FL_EndRegistration;
	e->RegisterModel = FL_RegisterModel;
	e->RegisterSkin = FL_RegisterSkin;
	e->RegisterShader = FL_RegisterShader;
	e->RegisterShaderNoMip = FL_RegisterShaderNoMip;
	e->RegisterServerModel = FL_RegisterServerModel;
	e->RefreshShaderNoMip = FL_RefreshShaderNoMip;
	e->RegisterFont = FL_RegisterFont;
	e->LoadFont = FL_LoadFont;

	ri.Cmd_AddCommand( "vkframelog", FL_Command_f );
	ri.Printf( PRINT_ALL, "r_vkFrameLog: recording the last %d frames, see vkframelog\n", fl.size );
	return e;
}
