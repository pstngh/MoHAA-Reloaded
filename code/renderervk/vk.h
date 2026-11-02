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

// vk.h -- interface between the renderer and its GPU backend
//
// The renderer (tr_*.c) contains no graphics API calls: every GPU operation
// goes through the functions declared here, and the vk*.c files implement
// them with Vulkan (see vk_local.h).
//
// The interface mirrors the fixed-function state the original GL1 renderer was
// written against, so rendering stays identical to it. State setters only
// record state; vk_draw() and vk_clear() consume the state current at the time
// of the call. A backend is expected to turn the recorded state into cached
// pipelines (see README.md in this directory for the full contract).
//
// Conventions (inherited from the GL1 renderer):
//  - matrices are 4x4, column-major; projection matrices produce clip space
//    with y up and z in [-w, w] (the backend remaps z, VK_ComputeMVP)
//  - window coordinates (viewport, scissor, read pixels) are in pixels with
//    the origin at the bottom-left corner of the window
//  - texture data is uploaded top row first; t = 0 is the first row
//  - front faces are counter-clockwise in window space

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
====================================================================

PLATFORM WINDOW (sdl/sdl_vkimp.c)

====================================================================
*/

void		VKimp_Init( void );			// create the window and fill glConfig video fields
void		VKimp_Shutdown( void );		// destroy the window
void		VKimp_EndFrame( void );		// handle window changes (fullscreen toggle) after presenting

// appended to the errors of a system without Vulkan (no loader, driver or
// device, no window with a Vulkan surface)
#define VK_DRIVER_HINT "; the game needs a graphics driver with Vulkan 1.1 or later"

/*
====================================================================

LIFECYCLE

====================================================================
*/

// called once the window exists; must fill glConfig with what the device
// supports: maxTextureSize, numTextureUnits (2), textureEnvAddAvailable,
// textureCompression (TC_S3TC_ARB when BC formats are supported),
// textureFilterAnisotropic/maxAnisotropy, the vendor/renderer/version/
// extensions strings, and depthBits/stencilBits if they differ from what
// VKimp_Init requested. The renderer then turns features off according to
// r_allowExtensions and the r_ext_* cvars, like the GL1 renderer did.
// The backend should also honour r_swapInterval (present mode) and
// r_ext_multisample.
void		vk_initialize( void );
// release every GPU resource; called before the window is destroyed
void		vk_shutdown( void );

// a frame is everything recorded between two vk_end_frame() calls;
// vk_begin_frame() is called at the start of each frame, but drawing,
// clearing and reading may also happen before it and must then start
// the frame implicitly
void		vk_begin_frame( void );
// submit the frame and present it
void		vk_end_frame( void );
// block until the GPU has finished all submitted work
void		vk_finish( void );

/*
====================================================================

TEXTURES

====================================================================
*/

typedef enum {
	TEXFMT_RGBA8,
	TEXFMT_RGB8,		// alpha is ignored and samples as 1
	TEXFMT_RGBA4,		// 16-bit storage requested by r_texturebits; RGBA8 is acceptable
	TEXFMT_RGB5,		// 16-bit storage requested by r_texturebits; RGB8 is acceptable
	TEXFMT_BC1_RGB,		// DXT1, pre-compressed data
	TEXFMT_BC1_RGBA,	// DXT1 with 1-bit alpha, pre-compressed data
	TEXFMT_BC2,			// DXT3, pre-compressed data
	TEXFMT_BC3			// DXT5, pre-compressed data
} texFormat_t;

// texture wrap mode, stored in image_t::wrapClampModeX/Y
#define WRAP_REPEAT		1
#define WRAP_CLAMP		2	// clamp to edge

// global filter for mipmapped textures (r_textureMode); textures without
// mipmaps always use linear filtering
typedef enum {
	TEXFILTER_NEAREST,
	TEXFILTER_LINEAR,
	TEXFILTER_NEAREST_MIPMAP_NEAREST,
	TEXFILTER_LINEAR_MIPMAP_NEAREST,
	TEXFILTER_NEAREST_MIPMAP_LINEAR,
	TEXFILTER_LINEAR_MIPMAP_LINEAR
} texFilter_t;

// (re)create the storage of an image: all mip levels, undefined contents;
// sets image->texnum to a non-zero backend handle, never reused for another
// image (the renderer caches bound handles). Calling it again on the
// same image replaces the previous storage (draws already recorded keep
// using the old one).
// Sampling uses image->wrapClampModeX/Y, the global texture filter and,
// for mipmapped textures, anisotropy from r_ext_texture_filter_anisotropic
// and r_ext_max_anisotropy.
void		vk_create_texture( image_t *image, int width, int height, int mipLevels, texFormat_t format );
// upload RGBA8 pixels (rows tightly packed, top row first) to a region of
// one mip level. Uploads may happen in the middle of a frame: draws recorded
// before the upload see the old contents, draws after it the new ones.
void		vk_upload_texture( image_t *image, int mipLevel, int x, int y, int width, int height, const byte *rgba );
// upload one complete mip level of a BC-compressed texture
void		vk_upload_compressed_texture( image_t *image, int mipLevel, int width, int height, const byte *data, int size );
// release the storage of an image and reset image->texnum to 0
void		vk_destroy_texture( image_t *image );
// minification filter of mipmapped textures; magnification is nearest for the
// TEXFILTER_NEAREST* modes and linear otherwise
void		vk_set_texture_filter( texFilter_t filter );

/*
====================================================================

RENDER STATE

All setters only record state; it applies to subsequent draws and clears.
R_SetDefaultState() sets every piece of state once the backend is
initialized, so the backend's own initial values do not matter.

====================================================================
*/

// shaderStage_t::multitextureEnv uses 0 for "no second texture", so the
// values start at 1
typedef enum {
	TEXENV_MODULATE = 1,	// result = previous * texture
	TEXENV_REPLACE,		// result = texture
	TEXENV_DECAL,		// result.rgb = mix(previous.rgb, texture.rgb, texture.a), result.a = previous.a
	TEXENV_ADD,			// result.rgb = previous.rgb + texture.rgb, result.a = previous.a * texture.a
	// GLS_MULTITEXTURE: rgb = vertex.rgb * vertex.a + texture.rgb * (1 - vertex.a),
	// a = previous.a * texture.a (no shader currently produces it)
	TEXENV_COMBINE_INTERPOLATE,
	// GLS_MULTITEXTURE_ENV (NV_texture_env_combine4, removed in MOHAA 2.0;
	// no shader currently produces it)
	TEXENV_COMBINE4
} texEnv_t;

typedef enum {
	CULL_NONE,
	CULL_FRONT,			// discard front faces
	CULL_BACK			// discard back faces
} cullFace_t;

typedef enum {
	STENCILFUNC_ALWAYS,
	STENCILFUNC_NOTEQUAL
} stencilFunc_t;

typedef enum {
	STENCILOP_KEEP,
	STENCILOP_INCR,		// increment and clamp
	STENCILOP_DECR		// decrement and clamp
} stencilOp_t;

// GLS_* bits (tr_local.h) for blending, depth test/func/write, polygon line
// mode and alpha test. The backend must honour GLS_SRCBLEND_BITS,
// GLS_DSTBLEND_BITS, GLS_DEPTHMASK_TRUE, GLS_DEPTHTEST_DISABLE,
// GLS_DEPTHFUNC_EQUAL (otherwise LEQUAL), GLS_POLYMODE_LINE and GLS_ATEST_BITS
// (the FOLIAGE thresholds come from r_alpha_foliage1/2), and should clamp
// both texture units to the edge while GLS_CLAMP_EDGE is set.
// Fog, color mask and multitexture bits are applied through the dedicated
// setters below and can be ignored here.
void		vk_set_state( unsigned long stateBits );

// texture units: unit 0 is combined with the vertex color, unit 1 with the
// result of unit 0. A unit only contributes when the geometry passed to
// vk_draw() has texture coordinates for it.
void		vk_bind_texture( int tmu, image_t *image );
// bindless textures: the slots of the units bound, unit 0 | unit 1 << 16 (the
// effects records, FX_TEX_SLOTS); 0 without them (vk.bindless)
uint32_t	vk_bound_texture_slots( void );
void		vk_set_texenv( int tmu, texEnv_t env );

void		vk_set_cull( cullFace_t face );
void		vk_set_depth_range( float zNear, float zFar );
void		vk_set_depth_clamp( qboolean enable );
#define VK_DEPTH_HACK_MAX	0.3f	// the depth range of the view weapon is [0, VK_DEPTH_HACK_MAX]
float		vk_depth_floor( void );	// the smallest depth of the full range [0, 1] draws
// depth bias for filled polygons only (not lines)
void		vk_set_polygon_offset( qboolean enable, float factor, float units );
void		vk_set_color_mask( qboolean red, qboolean green, qboolean blue, qboolean alpha );

// linear fog on eye-space depth, applied to the final fragment color
void		vk_set_fog( qboolean enable );
void		vk_set_fog_color( const vec4_t color );
void		vk_set_fog_range( float start, float end );

// user clip plane in eye space: keep fragments where dot(plane, eyePos) >= 0;
// NULL disables clipping
void		vk_set_clip_plane( const float *eyePlane );

void		vk_set_stencil_test( qboolean enable );
void		vk_set_stencil_func( stencilFunc_t func, int ref, unsigned int mask );
void		vk_set_stencil_op( stencilOp_t stencilFail, stencilOp_t depthFail, stencilOp_t depthPass );
void		vk_set_stencil_write_mask( unsigned int mask );

// Dynamic lights added per pixel to a lightmap (r_fastdlights 0), the way
// GL1's R_DlightSample added them to the lightmap's texels: at distance d of
// a light, color * ( 1 - d / radius )^2, truncated to a multiple of scale, is
// added to the lightmap texel (RGB, 1 = 255), then the sum is divided by its
// largest component when that is above 1. Positions are those of the draws'
// vertexes (object space).
typedef struct {
	float		origin[3];
	float		radius;
	float		color[3];
	float		scale;			// a lightmap byte before the overbright shift
} vkDlight_t;

// stores a list of lights for the draws of the current frame; returns its
// handle, valid until the frame ends, or -1 when the frame has no room left
// for them (the draws then get no light)
int			vk_add_dlights( int count, const vkDlight_t *dlights );
// the lights of a list (handle from vk_add_dlights) that the following draws
// add to the texture of unit tmu, a lightmap: mask has a bit for each light
// of the list (up to 64); tmu -1 turns it off. backs: no light from behind
// the plane of the triangle, whose front is the side its vertexes are
// clockwise from (the side a BSP face's plane normal points to; GL's front
// faces are the counter-clockwise ones)
void		vk_set_lightmap_dlights( int tmu, int handle, uint64_t mask, qboolean backs );

void		vk_set_line_width( float width );
// factor 0 disables stippling
void		vk_set_line_stipple( int factor, unsigned short pattern );

void		vk_set_projection( const float *matrix );
void		vk_set_modelview( const float *matrix );
void		vk_set_viewport( int x, int y, int width, int height );
// the scissor test is always enabled
void		vk_set_scissor( int x, int y, int width, int height );

/*
====================================================================

TEMPORAL PATH

With r_temporal (latched) the 3D views of a frame are a scene, drawn into
images of the backend's own (see vk_local.h, TEMPORAL PATH): HDR color,
motion vectors, normals, a depth pyramid, last frame's color and depth.
The renderer starts each 3D view of the world with vk_begin_scene; the
scene ends with vk_end_scene before anything else is drawn (2D, a view
without the world), or at the end of the frame, and is then drawn into the
frame. One scene per frame: a view that comes after the scene ended is
drawn as without the temporal path (vk_begin_scene returns qfalse).

Motion: a draw's last position is its vertexes through the last frame's
modelview (vk_set_prev_modelview, or by default the camera's motion only:
cameraDelta * modelview, a surface that doesn't move) and the view's last
projection; skinned draws use their entity's last bones too (vk.h, SKINNED
DRAWS). A view without prevProjection (a portal, a cut) has no motion.

====================================================================
*/

typedef struct {
	int				viewport[4];		// as vk_set_viewport
	const float		*projection;		// the view's projection, unjittered (the backend jitters it with r_temporalJitter)
	// the last frame's projection (unjittered), and its view matrix times
	// the inverse of this view's; NULL: the view has no last frame
	const float		*prevProjection;
	const float		*cameraDelta;
	qboolean		primary;			// the main view (not a portal or the sky portal)
	// the game time between the last frame and this one in milliseconds,
	// smoothed (the motion blur's exposure); 0 without a last frame
	float			frameMsec;
} vkSceneView_t;

// whether the temporal path is on (r_temporal and the device)
qboolean	vk_temporal_enabled( void );
// a 3D view of the world starts: begins the frame's scene, or continues it;
// qfalse when the view is drawn without the temporal path
qboolean	vk_begin_scene( const vkSceneView_t *view );
// ends the scene if one is being drawn (see above)
void		vk_end_scene( void );
// whether the primary view's ambient occlusion (r_ao) is still to come in
// this scene, and then it: between the view's opaque surfaces and what is
// blended over them; the fog of the view's draws, NULL for none: start,
// end, r, g, b (vk_set_fog_range, vk_set_fog_color)
qboolean	vk_ao_pending( void );
void		vk_ao_scene( const float *fog );
// the last frame's modelview of the following draws while the modelview is
// the current one (vk_set_modelview); NULL: the camera's motion only
void		vk_set_prev_modelview( const float *matrix );
// the following draws show what the motion vectors don't follow (effects,
// sprites, flares, animated textures): blended ones put their share of the
// pixel into the scene's alpha, where the anti-aliasing takes less history
// (r_taaReactive, VK_PIPE_REACTIVE); decals (polygon offset) never do.
// Only in a scene with the anti-aliasing and r_taaReactive (or
// r_temporalShow 14): otherwise the draws are as before
void		vk_set_reactive( qboolean reactive );

/*
====================================================================

DRAWING

====================================================================
*/

typedef enum {
	PRIM_TRIANGLES,
	PRIM_LINES
} primType_t;

// texture coordinate streams of the static geometry
typedef enum {
	STATIC_ST_NONE,			// the unit is off
	STATIC_ST_TEXTURE,
	STATIC_ST_LIGHTMAP,
	STATIC_ST_EFFECTS		// computed by the effects shader (staticFx)
} staticSt_t;

typedef struct {
	int				numVertexes;
	const float		*xyz;				// 3 floats per vertex
	int				xyzStride;			// in bytes
	const byte		*colors;			// RGBA8 per vertex, 4 bytes apart; NULL to use color
	float			color[4];			// constant color (0 to 1) when colors is NULL
	const float		*st[NUM_TEXTURE_BUNDLES];	// 2 floats per vertex; NULL disables the unit
	int				stStride[NUM_TEXTURE_BUNDLES];	// in bytes
	// 3 floats per vertex, or NULL: the temporal path's normals buffer
	// (read only in a scene, see TEMPORAL PATH)
	const float		*normals;
	int				normalStride;		// in bytes
	// 0, or a value that promises the same xyz contents as the previous
	// draw with the same version, xyz pointer, stride and vertex count
	// (and the same indexes when indexes and numIndexes are the same too):
	// the passes of a multi-pass shader draw the same vertexes, and the
	// backend may reuse what it copied for the previous pass
	unsigned int	version;

	// the vertexes come from the static geometry (vk_set_static_geometry)
	// instead of xyz, colors and st: vertex i of the indexes is vertex
	// staticVertex + i of the position and texture coordinate streams and
	// color staticColor + i of the color stream (the constant color when
	// staticColor is -1); staticSt selects the stream of each unit. With
	// staticFirstIndex >= 0 the indexes are numIndexes of the static index
	// stream from there, otherwise those given to vk_draw (reused like the
	// positions with version). With staticFx >= 0 the effects shader draws
	// it (shaders/effects.vert) with the record at word staticFx * 8 of the
	// frame data (vk_alloc_skin_frame), which says what each unit's texture
	// coordinates are (staticSt is only whether the unit is used) and has
	// the constant color when staticColor is -1. With staticTerrain the
	// streams are those of the terrain pool (vk_sync_terrain_pool) instead:
	// the indexes are its slots, staticColor 0 its white colors; never with
	// staticFx or staticFirstIndex
	qboolean		isStatic;
	qboolean		staticTerrain;
	int				staticVertex;
	int				staticColor;
	staticSt_t		staticSt[NUM_TEXTURE_BUNDLES];
	int				staticFirstIndex;
	int				staticFx;
	// with staticFx >= 0: its deforms move the vertexes over time, and the
	// record has their last frame's values too (the temporal path's motion
	// vectors, VK_PIPE_MOTION)
	qboolean		staticFxMotion;
	// a skinned draw (see SKINNED DRAWS below) instead of xyz, colors and
	// st: vertex i of the indexes is vertex record skinVertex + i of the
	// skin data, the colors and the texture coordinates of unit 0 (when
	// st[0] isn't NULL) come from the vertex shader, skinDraw is the draw
	// record (vk_alloc_skin_frame word / 8). With skinFirstIndex >= 0 the
	// indexes are numIndexes of the skin data from that word, otherwise
	// those given to vk_draw
	qboolean		isSkinned;
	int				skinVertex;
	int				skinDraw;
	int				skinFirstIndex;
} drawGeometry_t;

// draw indexed primitives with the current state; the arrays are only read
// during the call
void		vk_draw( primType_t prim, const drawGeometry_t *geometry, int numIndexes, const glIndex_t *indexes );

// Pipelines at load (tr_pipelines.c): what vk_draw would read of the state
// set above and of the geometry, for a draw that is expected later.
// vk_prebuild_pipeline creates the pipeline such a draw would use now, so
// the draw finds it, and returns whether it had to be created. Outside a
// frame too; it records nothing
typedef struct {
	primType_t		prim;
	unsigned long	stateBits;			// as given to vk_set_state
	cullFace_t		cull;
	qboolean		polygonOffset;
	qboolean		fog;
	qboolean		clipPlane;
	qboolean		depthClamp;			// as given to vk_set_depth_clamp (with the full depth range)
	texEnv_t		texEnv[NUM_TEXTURE_BUNDLES];
	image_t			*image[NUM_TEXTURE_BUNDLES];	// bound to the unit; NULL: the geometry has no texture coordinates for it
	int				dlightTmu;			// vk_set_lightmap_dlights' unit, -1 for none
	qboolean		dlightBacks;
	qboolean		colorArray;			// geometry colors per vertex (not the constant color)
	qboolean		skinned;
	qboolean		effects;			// from the static geometry with the effects shader (colorArray unused)
	// a blended draw of a 3D view as vk_set_reactive will have it: 0 not
	// reactive, 1 reactive, 2 both can happen (built twice)
	int				reactive;
} vkDrawState_t;

qboolean	vk_prebuild_pipeline( const vkDrawState_t *state );
// a registration starts: the pipelines built for the last one that no draw
// used are destroyed
void		vk_begin_prebuild( void );
// its shaders' pipelines are built: those recorded in earlier runs too (the
// keys that came after a load: see vk_pipeline.c), and what
// vk_prebuild_pipeline builds from now on is recorded for the next runs
void		vk_end_prebuild( void );

// Static geometry: what doesn't change after a map load (world surfaces,
// static models) is uploaded once into device memory and drawn from there.
// Positions and texture coordinates share one vertex numbering, colors have
// their own (instances of a static model share positions, not colors).
typedef struct {
	int				numVertexes;
	const float		*xyz;				// 3 floats per vertex
	const float		*st[2];				// 2 floats per vertex: STATIC_ST_TEXTURE, STATIC_ST_LIGHTMAP
	const float		*normals;			// 3 floats per vertex, for the effects shader
	int				numColors;
	const byte		*colors;			// RGBA8
	int				numIndexes;
	const glIndex_t	*indexes;
} staticGeometry_t;

// replace the static geometry; NULL releases it. Draws recorded before
// keep the old one
void		vk_set_static_geometry( const staticGeometry_t *geometry );

// Terrain pool: the vertexes of the LOD terrain (tr_terrain.c) live in the
// slots of a pool, whose positions change from frame to frame (splits and
// merges fill and free slots, the geomorphs move them up and down), while
// most slots stay as they are. Every frame slot of the backend has its own
// copy of the pool's positions and texture coordinates in host visible
// memory (white colors: terrain has no vertex colors), and a frame brings
// its copy up to date before its first draw from it, writing the slots
// that changed since that copy was last written: no copy of every terrain
// vertex per frame, no transfer or barrier (the copy belongs to the frame).
//
// vk_set_terrain_pool: copies for numSlots slots, 0 releases them; every
// slot is written at each copy's next update. Draws recorded before keep
// the old copies.
// vk_sync_terrain_pool: changed has a bit per slot (bit s & 63 of word
// s >> 6) whose position or texture coordinates changed since the last
// call, which it clears (anyChanged: whether there is one), xyz and st
// (texture, then lightmap coordinates) are those of slot 0, stride bytes
// apart. Brings the current frame's copy up to date and returns qtrue, or
// qfalse when there is no pool of numSlots slots, or when a draw of this
// frame reads its copy already and slots changed since (they would change
// under that draw): such a draw takes its vertexes from tess
void		vk_set_terrain_pool( int numSlots );
qboolean	vk_sync_terrain_pool( int numSlots, const float *xyz, const float *st, int stride, uint64_t *changed, qboolean anyChanged );

/*
====================================================================

SKINNED DRAWS

Skeletal models skinned and lit in the vertex shader (shaders/skinned.vert,
tr_skin.c). The vertexes of their surfaces don't change: they are added
once to the skin data. What changes every frame (bones, morph weights,
lights) goes into the frame data, and each draw has a record there. Both
are arrays of 32-bit words.

Skin data: vertex records of 8 words (normal xyz, st, numWeights |
numMorphs << 16, how many words after the record's first the vertex's
morphs start, 4 words each: morph target, offset xyz, followed by its
weights, 5 words each: bone, weight, offset xyz; the vertex it collapses
to at a lower level of detail, in the surface), bone remap tables (bone of
a mesh to bone of the entity) and indexes.

Frame data: draw records (vkSkinDraw_t), the bones of each entity
(skelBoneCache_t, 16 words each) after the view origin in its space (4
words, for environment mapped passes), its morph weights (ints), and light
lists (count, ambient RGBA8, then a vkSkinLight_t per light).

====================================================================
*/

#define VK_SKIN_NONE	0xFFFFFFFFu

// in a scene of the temporal path (vk_begin_scene), the 4 words before an
// entity's view origin in the frame data: the first word of its bones and
// of its morph weights in the last frame's frame data (VK_SKIN_NONE for
// none: no motion of the bones), then two unused
#define VK_SKIN_PREV_WORDS	4

// the texture coordinates of a skinned draw (vkSkinDraw_t::firstVertex):
// the mesh's, RB_CalcEnvironmentTexCoords', RB_CalcEnvironmentTexCoords2's
#define VK_SKIN_ENV_SHIFT	30
#define VK_SKIN_ENV_NONE	0u
#define VK_SKIN_ENV_MAPPED	1u
#define VK_SKIN_ENV_MAPPED2	2u

typedef struct {
	uint32_t		bones;		// first word of the entity's bones
	uint32_t		remap;		// first word of the mesh's bone remap table in the skin data, or VK_SKIN_NONE
	uint32_t		morphs;		// first word of the entity's morph weights, or VK_SKIN_NONE
	uint32_t		lights;		// first word of the light list, or VK_SKIN_NONE for color
	float			scale;
	uint32_t		color;		// RGBA8, without a light list
	uint32_t		renderCount;	// level of detail: the vertexes from there are collapsed (vertex record word 7)
	uint32_t		firstVertex;	// the surface's first vertex record, VK_SKIN_ENV_* << VK_SKIN_ENV_SHIFT
} vkSkinDraw_t;

typedef struct {
	uint32_t		type;		// lighttype_t
	float			color[3];
	float			origin[3];
	float			direction[3];
	float			spotConst;
	float			spotScale;
	uint32_t		unused[4];
} vkSkinLight_t;

// whether the device draws skinned draws
qboolean	vk_skin_available( void );
// whether the device draws static geometry with the effects shader
// (drawGeometry_t::staticFx)
qboolean	vk_effects_available( void );
// adds words to the skin data; returns the first one, a multiple of 8, or
// -1 when there is no room left. Draws recorded later in the frame can use them
int			vk_add_skin_data( const uint32_t *words, int numWords );
// forgets the skin data (a new map: the models it was made from may be
// freed); waits for the GPU
void		vk_clear_skin_data( void );
// space for numWords words in the frame data of the current frame: returns
// the first word, a multiple of 8, and where to write them, or -1 when the
// frame has no room left
int			vk_alloc_skin_frame( int numWords, uint32_t **data );
// whether numWords more words fit in the current frame's frame data (each
// allocation starts at a multiple of 8: count its size rounded up to 8)
qboolean	vk_skin_frame_room( int numWords );
// a number that changes with every frame (the frame data starts empty);
// starts the frame
unsigned int vk_frame_serial( void );

#define CLEAR_COLOR		1
#define CLEAR_DEPTH		2	// to 1.0
#define CLEAR_STENCIL	4	// to 0

// clear the current scissor rectangle; like glClear it honours the color
// mask, the depth write bit of the current state and the stencil write mask.
// color may be NULL when CLEAR_COLOR is not requested
void		vk_clear( int buffers, const vec4_t color );

typedef enum {
	READ_RGB8,			// 3 bytes per pixel
	READ_DEPTH_FLOAT,	// window-space depth in [0, 1], one float per pixel
	READ_STENCIL8		// 1 byte per pixel
} readFormat_t;

// read back what has been drawn so far in the current frame; rows are
// tightly packed and stored bottom row first. This waits for the GPU. Like
// a GL back buffer, the color contents are undefined before the frame has
// drawn or cleared anything (the renderer takes screenshots at the end of
// the frame, RB_TakePendingScreenshots). Depth and stencil read as 0 until
// the depth buffer is readable, see vk_want_depth_reads.
void		vk_read_pixels( int x, int y, int width, int height, readFormat_t format, void *buffer );
// the latency probe (r_vkLatency): the main thread's EndFrame (it returns
// the end of the last one, when the next frame's input is read after) and
// the back end's frame (the input time its swap command carries)
uint64_t	vk_probe_input_time( void );
void		vk_probe_frame_end( void );
void		vk_probe_input( uint64_t inputUs );
// the same without waiting: the back end records the copy and gets a handle
// (0: too many in flight, dropped); the main thread takes the readbacks
// once the GPU has done them, in the order they were recorded, with the
// caller's kind and user data (at most 2 * MAX_OSPATH + 256 bytes): vk_readback_next gives
// the oldest when it is done, vk_readback_get its pixels (as vk_read_pixels
// writes them, for the rectangle asked for), vk_readback_free returns it.
// vk_finish completes every submitted frame's readbacks
int			vk_read_pixels_async( int x, int y, int width, int height, readFormat_t format, int kind, const void *user, int userSize );
int			vk_readback_next( int *kind, void **user );
void		vk_readback_get( int handle, void *buffer );
void		vk_readback_free( int handle );
int			vk_readback_dropped( void );
// the frame being recorded will be read back (a screenshot, a video frame):
// with HDR output its 3D scene is drawn without the expansion, so the
// readback is the SDR frame (vk_output.c)
void		vk_sdr_frame( void );

// Depth and stencil reads (vk_read_pixels with READ_DEPTH_FLOAT or
// READ_STENCIL8, vk_queue_depth_reads) need a depth buffer that some
// drivers draw into more slowly (RADV keeps fewer depth tiles compressed in
// a depth image that can be copied from). Without multisampling the
// backend draws into such a depth buffer only from the start of the frame
// after a read or a call to vk_want_depth_reads, until a few hundred frames
// (VK_DEPTH_READ_FRAMES) after the last one; until then depth and stencil
// read as 0 and vk_queue_depth_reads gives ids 0. Called between frames,
// vk_want_depth_reads makes the reads of the next frame work
// (r_measureOverdraw calls it in RE_BeginFrame).
void		vk_want_depth_reads( void );

typedef enum {
	DEPTHREAD_PENDING,	// the GPU has not executed the read yet
	DEPTHREAD_DONE,		// the depth is available
	DEPTHREAD_LOST		// unknown or expired id
} depthReadStatus_t;

// record reads of the window-space depth at count window positions (x, y
// pairs), of what has been drawn so far in the current frame, without
// waiting for the GPU; fills ids (0 when the frame has no room left, or the
// depth buffer isn't readable yet: see vk_want_depth_reads). Positions
// outside the framebuffer read as 0.
void		vk_queue_depth_reads( int count, const int *xy, unsigned int *ids );
// result of a queued read: DEPTHREAD_DONE once the frame that recorded it
// has completed, which the backend checks when a later frame starts (a fixed
// number of frames later: 2, the frames in flight). Results stay available
// for that many frames; after that, and after vid_restart, the id is lost.
depthReadStatus_t vk_get_depth_read( unsigned int id, float *depth );

// the frame log (vk_framelog.c, r_vkFrameLog): called by GetRefAPI with the
// renderer's exports, returns them, or wrappers that time them when
// r_vkFrameLog is set
refexport_t	*VK_FrameLogExports( refexport_t *real );

#ifdef __cplusplus
}
#endif
