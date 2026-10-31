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
// tr_mtl.h -- the Metal backend API used by the renderer front end.
//
// renderer_metal is a copy of upstream's GL1 renderer (code/renderergl1, deleted in Phase 4) in which every
// GL call became an mb call with the same arguments (glFoo -> mbFoo) and every GL constant an MB_ constant
// of the same name and value. The backend (tr_mtl.m, Objective-C) mirrors the GL1 fixed-function state model
// the front end was written against (caps, blend/depth/alpha/stencil state, texenv per texture unit, client
// arrays, matrices, fog, clip plane) and turns it into Metal pipeline/depth-stencil/sampler state at draw
// time. That keeps visual parity with GL1 and keeps upstream GL1 renderer fixes portable file by file
// (the other renames are listed in docs/METAL_PORT_PLAN.md, Phase 4).
//
// The MB_ values are numerically equal to the GL enums they replace, so shader/texture data, decompiled
// literals and printouts (imagelist, gfxinfo) stay identical. They are plain numbers.

#ifndef TR_MTL_H
#define TR_MTL_H

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

//
// enums (same values as GL1)
//
#define MB_FALSE						0
#define MB_TRUE							1
#define MB_NO_ERROR						0

// primitives
#define MB_POINTS						0x0000
#define MB_LINES						0x0001
#define MB_LINE_LOOP					0x0002
#define MB_LINE_STRIP					0x0003
#define MB_TRIANGLES					0x0004
#define MB_TRIANGLE_STRIP				0x0005
#define MB_TRIANGLE_FAN					0x0006
#define MB_QUADS						0x0007
#define MB_POLYGON						0x0009

// mbClear bits
#define MB_DEPTH_BUFFER_BIT				0x0100
#define MB_STENCIL_BUFFER_BIT			0x0400
#define MB_COLOR_BUFFER_BIT				0x4000

// compare functions
#define MB_NEVER						0x0200
#define MB_LESS							0x0201
#define MB_EQUAL						0x0202
#define MB_LEQUAL						0x0203
#define MB_GREATER						0x0204
#define MB_NOTEQUAL						0x0205
#define MB_GEQUAL						0x0206
#define MB_ALWAYS						0x0207

// blend factors
#define MB_ZERO							0
#define MB_ONE							1
#define MB_SRC_COLOR					0x0300
#define MB_ONE_MINUS_SRC_COLOR			0x0301
#define MB_SRC_ALPHA					0x0302
#define MB_ONE_MINUS_SRC_ALPHA			0x0303
#define MB_DST_ALPHA					0x0304
#define MB_ONE_MINUS_DST_ALPHA			0x0305
#define MB_DST_COLOR					0x0306
#define MB_ONE_MINUS_DST_COLOR			0x0307
#define MB_SRC_ALPHA_SATURATE			0x0308

// faces, draw buffers
#define MB_BACK_LEFT					0x0402
#define MB_BACK_RIGHT					0x0403
#define MB_FRONT						0x0404
#define MB_BACK							0x0405
#define MB_FRONT_AND_BACK				0x0408

// mbEnable/mbDisable caps
#define MB_LINE_STIPPLE					0x0B24
#define MB_CULL_FACE					0x0B44
#define MB_FOG							0x0B60
#define MB_DEPTH_TEST					0x0B71
#define MB_STENCIL_TEST					0x0B90
#define MB_ALPHA_TEST					0x0BC0
#define MB_BLEND						0x0BE2
#define MB_SCISSOR_TEST					0x0C11
#define MB_TEXTURE_2D					0x0DE1
#define MB_CLIP_PLANE0					0x3000
#define MB_POLYGON_OFFSET_FILL			0x8037
#define MB_DEPTH_CLAMP					0x864F	// GL's depth clamp: only with the full depth range (mbDepthFloor)

// client arrays
#define MB_VERTEX_ARRAY					0x8074
#define MB_COLOR_ARRAY					0x8076
#define MB_TEXTURE_COORD_ARRAY			0x8078

// data types
#define MB_UNSIGNED_BYTE				0x1401
#define MB_UNSIGNED_INT					0x1405
#define MB_FLOAT						0x1406

// pixel and texture formats
#define MB_STENCIL_INDEX				0x1901
#define MB_DEPTH_COMPONENT				0x1902
#define MB_RGB							0x1907
#define MB_RGBA							0x1908
#define MB_RGB5							0x8050
#define MB_RGB8							0x8051
#define MB_RGBA4						0x8056
#define MB_RGBA8						0x8058
#define MB_BGR							0x80E0
#define MB_BGR_EXT						0x80E0
#define MB_RGB4_S3TC					0x83A1
#define MB_COMPRESSED_RGB_S3TC_DXT1_EXT		0x83F0
#define MB_COMPRESSED_RGBA_S3TC_DXT1_EXT	0x83F1
#define MB_COMPRESSED_RGBA_S3TC_DXT3_EXT	0x83F2
#define MB_COMPRESSED_RGBA_S3TC_DXT5_EXT	0x83F3

// polygon and shade modes
#define MB_LINE							0x1B01
#define MB_FILL							0x1B02
#define MB_FLAT							0x1D00
#define MB_SMOOTH						0x1D01

// stencil ops
#define MB_KEEP							0x1E00
#define MB_INCR							0x1E02
#define MB_DECR							0x1E03

// texture environment modes (mbTexEnv). MB_COMBINE is the one combiner setup GL1 used
// (R_State, GLS_MULTITEXTURE): rgb = mix(texture, primary color, primary alpha), alpha = texture * previous.
// MB_COMBINE4_NV is GLS_MULTITEXTURE_ENV; Apple's GL had no NV_texture_env_combine4, so GL1's glTexEnv
// call failed there (invalid enum) and the unit kept its previous mode. mbTexEnv does the same.
#define MB_ADD							0x0104
#define MB_REPLACE						0x1E01
#define MB_MODULATE						0x2100
#define MB_DECAL						0x2101
#define MB_COMBINE4_NV					0x8503
#define MB_COMBINE						0x8570

// matrix modes
#define MB_MODELVIEW					0x1700
#define MB_PROJECTION					0x1701

// texture units
#define MB_TEXTURE0						0x84C0
#define MB_TEXTURE1						0x84C1
#define MB_TEXTURE0_ARB					MB_TEXTURE0
#define MB_TEXTURE1_ARB					MB_TEXTURE1

// texture parameters (mbTexParameterf/i on the texture bound to the active unit)
#define MB_TEXTURE_MAG_FILTER			0x2800
#define MB_TEXTURE_MIN_FILTER			0x2801
#define MB_TEXTURE_WRAP_S				0x2802
#define MB_TEXTURE_WRAP_T				0x2803
#define MB_TEXTURE_MAX_ANISOTROPY_EXT	0x84FE
#define MB_NEAREST						0x2600
#define MB_LINEAR						0x2601
#define MB_NEAREST_MIPMAP_NEAREST		0x2700
#define MB_LINEAR_MIPMAP_NEAREST		0x2701
#define MB_NEAREST_MIPMAP_LINEAR		0x2702
#define MB_LINEAR_MIPMAP_LINEAR			0x2703
#define MB_CLAMP						0x2900
#define MB_REPEAT						0x2901
#define MB_CLAMP_TO_EDGE				0x812F

// fog (linear distance fog only)
#define MB_FOG_START					0x0B63
#define MB_FOG_END						0x0B64
#define MB_FOG_MODE						0x0B65
#define MB_FOG_COLOR					0x0B66

// error codes (R_CheckErrors); the backend reports none
#define MB_INVALID_ENUM					0x0500
#define MB_INVALID_VALUE				0x0501
#define MB_INVALID_OPERATION			0x0502
#define MB_STACK_OVERFLOW				0x0503
#define MB_STACK_UNDERFLOW				0x0504
#define MB_OUT_OF_MEMORY				0x0505

//
// device, window and frame (called by sdl/sdl_mtlimp.m)
//
// The backend never waits for the GPU between input sampling and encoding: the frames-in-flight wait
// happens in mbEndFrame, after the frame is committed, and the drawable is taken only there. Drawing and
// clearing may happen at any point of a frame (the 2D exports draw outside the render-command queue).
qboolean	mbInit( void *metalLayer );		// CAMetalLayer *; creates the device/queue once, binds the layer
void		mbShutdown( void );					// waits for the GPU, releases the layer and per-window objects
void		mbSetGamma( const unsigned char red[256], const unsigned char green[256], const unsigned char blue[256] );	// applied in the final pass
void		mbEndFrame( void );					// final pass into the drawable, present, commit, frame pacing
const char	*mbRendererString( void );			// e.g. "Apple M4 (Metal)"
int			mbMaxTextureSize( void );

// the cl_latencyStats probe (re.LatencyTag/re.LatencyPoll, see renderercommon/tr_public.h)
void		mbLatencyTag( int tag );
qboolean	mbLatencyPoll( int *tag, long long *commitUsec, long long *gpuDoneUsec, long long *presentUsec );

// re.WakeGPU: a frame starts (right after the engine's frame limiter); wakes the GPU if it has been idle
void		mbWakeGPU( void );

extern qboolean	mbMultitexture;				// ARB_multitexture equivalent (r_ext_multitexture)

// Pipelines ahead of their first draw: when a shader is loaded, the front end describes the state each of its stages
// draws with (R_PrecacheState), and the backend creates the pipelines it doesn't have yet, compiling them on Metal's
// compiler threads (a variant the system shader cache doesn't have takes 10-70 ms). A draw that needs one still
// compiling waits for it; mbPrecacheFinish waits for all of them, at the end of a level's registration, so none is
// compiled in play.
void	mbPrecachePipeline( int numUnits, qboolean alphaTest, qboolean blend, int sfactor, int dfactor, qboolean skin );
void	mbPrecacheFinish( void );

//
// state
//
void	mbEnable( int cap );
void	mbDisable( int cap );
void	mbBlendFunc( int sfactor, int dfactor );
void	mbDepthFunc( int func );
void	mbDepthMask( qboolean flag );
void	mbDepthRange( double zNear, double zFar );
// r_depthClamp: the full depth range [0, 1] starts at floor instead (the view weapon's depth hack range ends there),
// so a surface MB_DEPTH_CLAMP keeps nearer than the near plane doesn't hide the weapon; depth queries follow it
void	mbDepthFloor( float floor );
void	mbAlphaFunc( int func, float ref );
void	mbCullFace( int mode );
void	mbPolygonMode( int face, int mode );
void	mbPolygonOffset( float factor, float units );
void	mbColorMask( qboolean red, qboolean green, qboolean blue, qboolean alpha );
void	mbStencilFunc( int func, int ref, unsigned int mask );
void	mbStencilOp( int fail, int zfail, int zpass );
void	mbStencilMask( unsigned int mask );
void	mbShadeModel( int mode );
void	mbLineWidth( float width );
void	mbLineStipple( int factor, unsigned short pattern );
void	mbFogf( int pname, float param );
void	mbFogfv( int pname, const float *params );
void	mbFogi( int pname, int param );
void	mbClipPlane( int plane, const double *equation );	// eye space, transformed by the current modelview
void	mbViewport( int x, int y, int width, int height );	// GL window coordinates (origin bottom-left)
void	mbScissor( int x, int y, int width, int height );
void	mbDrawBuffer( int mode );

//
// clears
//
void	mbClearColor( float red, float green, float blue, float alpha );
void	mbClearDepth( double depth );
void	mbClearStencil( int s );
void	mbClear( unsigned int mask );

//
// matrices (GL conventions; the backend remaps clip z to [0,1] and flips y)
//
void	mbMatrixMode( int mode );
void	mbLoadMatrixf( const float *m );
void	mbLoadIdentity( void );
void	mbOrtho( double left, double right, double bottom, double top, double zNear, double zFar );
void	mbPushMatrix( void );
void	mbPopMatrix( void );
void	mbTranslatef( float x, float y, float z );

//
// textures. Names are indices into the backend's texture table and are never reused while a frame
// that may still reference them is in flight.
//
void	mbActiveTextureARB( int texture );
void	mbClientActiveTextureARB( int texture );
void	mbGenTextures( int n, unsigned int *textures );
void	mbDeleteTextures( int n, const unsigned int *textures );
void	mbBindTexture( int target, unsigned int texture );
void	mbTexEnv( int mode );					// texture environment of the active unit (MB_MODULATE, ...)
void	mbTexParameterf( int target, int pname, float param );
void	mbTexParameteri( int target, int pname, int param );
void	mbTexImage2D( int target, int level, int internalformat, int width, int height, int border, int format, int type, const void *pixels );
void	mbTexSubImage2D( int target, int level, int xoffset, int yoffset, int width, int height, int format, int type, const void *pixels );
void	mbCompressedTexImage2D( int target, int level, int internalformat, int width, int height, int border, int imageSize, const void *data );

//
// client arrays and indexed draws. Array data is read at draw time (copied into the frame's buffer ring),
// so the front end may reuse its memory right after the call returns. Lock/Unlock bracket draws that
// share the same vertices (one copy for all stages of a shader).
//
void	mbEnableClientState( int array );
void	mbDisableClientState( int array );
void	mbVertexPointer( int size, int type, int stride, const void *pointer );
void	mbColorPointer( int size, int type, int stride, const void *pointer );
void	mbTexCoordPointer( int size, int type, int stride, const void *pointer );
void	mbLockArraysEXT( int first, int count );
void	mbUnlockArraysEXT( void );
void	mbDrawElements( int mode, int count, int type, const void *indices );
void	mbArrayElement( int i );

//
// Static vertex data (the world's faces): copied once into a GPU buffer and kept until mbStaticReset. While
// mbStaticArrays( qtrue ) is in effect, the pointers given to mbVertexPointer, mbColorPointer and mbTexCoordPointer
// are byte offsets into that buffer (like arrays in a bound GL vertex buffer object) and the indices of
// mbDrawElements count vertexes from there, so a draw copies only its indices. mbArrayElement and the immediate
// mode don't read the static buffer.
//
int			mbStaticData( const void *data, int bytes );	// -> byte offset (> 0, 16-byte aligned), -1 if it doesn't fit
void		mbStaticReset( void );						// every offset is dropped (the world was freed)
qboolean	mbStaticArrays( qboolean on );				// qfalse: there is no static data, nothing changed

//
// GPU skinning of skeletal (TIKI) surfaces. Static data (a surface's vertex records, a mesh's bone map) is copied
// once into a GPU buffer; a draw made between mbSkinBegin and mbSkinEnd then computes positions, texcoord 0 and,
// with lights, the colors in the vertex shader (tr_mtl_stage.metal mb_skin_vs) instead of reading the vertex,
// texcoord 0 and color arrays. The shader repeats the CPU's arithmetic operation for operation (RB_SkelMesh,
// RB_Light_Real), so it gives the same bits.
//
// Vertex records as the model holds them: skeletorVertex_t, then its morphs (skeletorMorph_t), then its weights
// (skelWeight_t); a surface's static data is one word per vertex (that vertex's record, in words from the start
// of the data) followed by the records. Lights are reallightinfo_t, bones skelBoneCache_t.
//
#define MB_SKIN_MAX_JOBS	16
#define MB_SKIN_MAX_LIGHTS	32

int		mbSkinStatic( const void *data, int bytes );	// -> handle >= 0 (a word offset), -1 if it doesn't fit
void	mbSkinReset( void );					// every handle is dropped (the models were freed)

typedef struct {
	int				firstVertex;	// the draw's vertex index of the surface's vertex 0; jobs are in vertex order
	int				numVertexes;
	int				surface;		// mbSkinStatic handle of the vertex records
	int				boneMap;		// mbSkinStatic handle of a weight bone -> bone table (meshes > 0), -1: none
	const void		*bones;			// numBones skelBoneCache_t, read at the draw
	int				numBones;
	const int		*morphs;		// numMorphs morph weights, NULL: the entity has no morphs
	int				numMorphs;
	float			scale;
} mbSkinJob_t;

typedef struct {
	int					numJobs;	// 1..MB_SKIN_MAX_JOBS
	const mbSkinJob_t	*jobs;
	int					numLights;	// 0..MB_SKIN_MAX_LIGHTS sphere lights, or -1: colors from the color array
	const void			*lights;	// numLights reallightinfo_t
	byte				ambient[4];
	unsigned			scene;		// bones and morphs are copied once per scene: same scene and pointer = same data
} mbSkinDraw_t;

// The mbDrawElements calls until mbSkinEnd are skinned (nothing else may be drawn in between); the descriptor and
// what it points to are read at each of them. qfalse: GPU skinning is unavailable, nothing changed.
qboolean	mbSkinBegin( const mbSkinDraw_t *draw );
void		mbSkinEnd( void );

//
// immediate mode (debug views, sky, flares, shadows, 2D boxes and lines)
//
void	mbBegin( int mode );
void	mbEnd( void );
void	mbVertex2f( float x, float y );
void	mbVertex3f( float x, float y, float z );
void	mbVertex3fv( const float *v );
void	mbTexCoord2f( float s, float t );
void	mbTexCoord2fv( const float *v );
void	mbMultiTexCoord2fARB( int target, float s, float t );
void	mbColor3f( float red, float green, float blue );
void	mbColor4f( float red, float green, float blue, float alpha );
void	mbColor4ubv( const byte *v );

//
// synchronization and readback
//
void	mbFinish( void );						// doesn't wait (see tr_mtl.m): queueing is bounded by r_metalFramesInFlight
void	mbReadPixels( int x, int y, int width, int height, int format, int type, void *pixels );	// MB_RGB/MB_RGBA, rows tightly packed, bottom row first; waits for the GPU
void	mbDrawPixels( int width, int height, int format, int type, const void *pixels );
void	mbPixelZoom( float xfactor, float yfactor );

// Readback without waiting: the copy is recorded at this point of the frame (what has been drawn so far) and
// done() gets the pixels once the GPU has finished the frame, from a later mbEndFrame (after that frame's
// present, before the next frame's input sampling) or from mbFlushReadbacks. Pixels as mbReadPixels returns
// them, MB_UNSIGNED_BYTE: format MB_RGB, MB_RGBA or MB_STENCIL_INDEX. They are valid during the call only, and
// done() may modify them.
typedef void ( *mbReadbackFunc_t )( byte *pixels, int width, int height, void *arg );
qboolean	mbReadPixelsAsync( int x, int y, int width, int height, int format, mbReadbackFunc_t done, void *arg );	// qfalse: done() is never called
void		mbFlushReadbacks( void );		// waits for the pending readbacks and runs their callbacks

// Depth queries without waiting (Q3 flares): a one-pixel point at window position x, y (origin bottom-left)
// with the given window depth, depth-tested (less) against what has been drawn so far, nothing written.
// mbDepthQuery returns a handle (0: none issued). mbDepthQueryResult returns 1 if the point passed, 0 if not,
// -1 while its frame is still in flight (answered from r_metalFramesInFlight frames later), -2 if it is lost.
int		mbDepthQuery( int x, int y, float depth );
int		mbDepthQueryResult( int handle );

static ID_INLINE int mbGetError( void ) { return MB_NO_ERROR; }

#ifdef __cplusplus
}
#endif

#endif // TR_MTL_H
