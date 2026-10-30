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

// vk_temporal.c -- the temporal path (r_temporal): what temporal
// anti-aliasing and screen space ambient occlusion need from a frame
//
// A scene (the frame's 3D views of the world, vk_begin_scene in vk.c) draws
// into images of its own, with render passes of its own (renderPassClear
// begins the scene and clears the color and the normals, renderPass
// continues it):
//  - color[current]: RGBA16F
//  - normal: A2B10G10R10, the view space normal (octahedral, rg), flags in
//    b (TEMPORAL_FLAG_*: a draw gave the normal, it moves), the class in
//    the 2 alpha bits (VK_TEMPORAL_CLASS_*)
//  - velocity: RG16F, this frame's position of the surface minus the last
//    frame's, in units of the viewport (x right, y down), where the flag
//    TEMPORAL_FLAG_MOTION says a draw that moves wrote it
//  - vk.depthImage, made sampled
// Only the pipelines that write depth without blending write the normals
// (VK_TemporalWritesBuffers), and of those only the ones that draw what
// moves (VK_PIPE_MOTION: entities with a last frame, skeletal models) the
// motion vectors. Translucent surfaces leave the buffers of the surface
// behind them. What the buffers don't hold, their readers work out from the
// depth (shaders/temporal_common.glsl): the camera's motion of what doesn't
// move, the normals the draws didn't give.
//
// When the scene ends (VK_TemporalResolve) shaders/hiz_build.comp makes the
// depth pyramid hiz[current] (view distances, the minimum and the maximum
// of what a texel covers, level 0 at half the framebuffer's size): levels 0
// and 1 from the depth buffer in one dispatch (PREPARE), then two levels per
// dispatch, then the small ones that are left in one (TAIL; vkt.hizPasses).
// Only when a reader needs it (VK_TEMPORAL_READ_DEPTH): it costs about as
// much as the composite.
// Then what resolves the scene (VK_TemporalConsumers): with r_taa temporal
// anti-aliasing (shaders/taa_resolve.comp) blends the scene with the last
// frame's result into taa[current]; when it is the pyramid's only reader it
// makes level 0 itself (the only level it reads) and the pyramid's passes
// are left out. Then the scene (or that result) is drawn
// into the frame's color target (VK_TemporalComposite,
// shaders/temporal_composite.frag), where the 2D drawing follows. The other
// color image, pyramid and anti-aliased image hold the last frame's: the
// history (level 0 of the pyramid is the last frame's depth).
//
// With r_motionBlur, after the anti-aliasing (VK_TemporalMotionBlur):
// shaders/motion_blur.comp gives each pixel its blur (its motion: the draw's
// motion vector, the camera's from the depth, scaled to the exposure, as a
// half vector in pixels) and its view distance, and each tile the longest of
// its pixels' and then of the 3 x 3 tiles around it; the composite gathers
// the scene along them (temporal_composite.frag, MOTION_BLUR), so the
// anti-aliasing's history stays unblurred.
//
// With r_dof, before that (VK_TemporalDof): shaders/dof.comp eases the
// focus toward what is under the crosshair, gives each pixel its blur (a
// thin lens' circle of confusion from its view distance), gathers the
// background and the foreground apart at half the resolution and puts them
// over the sharp scene into dofResult, which the composite shows instead (and
// the motion blur gathers from); the history stays sharp here too.
//
// Before that, in the primary view between its opaque surfaces and what is
// blended over them, the ambient occlusion (r_ao, VK_TemporalAo, called by
// vk_ao_scene): the scene's render pass ends, compute passes make view
// distances (shaders/ao_prefilter.comp), the occlusion (gtao_main.comp),
// blend it with the last frames' (ao_temporal.comp) and blur it
// (ao_denoise.comp), and the next render pass blends it into the color
// first (ao_apply.frag); its images stay GENERAL.
//
// Layouts between scenes: the color, motion vector and normal images
// SHADER_READ_ONLY_OPTIMAL, the pyramids, taa and the motion blur's GENERAL, the depth image
// DEPTH_STENCIL_ATTACHMENT_OPTIMAL as always. The images the scene draws
// into are never storage images (which can cost them their compression).

#include "vk_local.h"

vkTemporal_t	vkt;

cvar_t	*r_temporal;
cvar_t	*r_temporalJitter;
cvar_t	*r_temporalJitterSamples;
cvar_t	*r_temporalShow;
cvar_t	*r_temporalShowMip;
cvar_t	*r_temporalShowScale;
cvar_t	*r_taa;
cvar_t	*r_taaFeedback;
cvar_t	*r_taaSharpen;
cvar_t	*r_saturation;
cvar_t	*r_ao;
cvar_t	*r_aoQuality;
cvar_t	*r_aoRadius;
cvar_t	*r_aoStrength;
cvar_t	*r_aoFalloff;
cvar_t	*r_aoThickness;
cvar_t	*r_aoTemporal;
cvar_t	*r_aoNormals;
cvar_t	*r_aoMultiBounce;
cvar_t	*r_motionBlur;
cvar_t	*r_motionBlurShutter;
cvar_t	*r_motionBlurFps;
cvar_t	*r_motionBlurCamera;
cvar_t	*r_motionBlurQuality;
cvar_t	*r_dof;
cvar_t	*r_dofAperture;
cvar_t	*r_dofFocus;
cvar_t	*r_dofFocusSpeed;
cvar_t	*r_dofQuality;

// push constants of the passes, see the shaders
typedef struct {
	vkTemporalView_t	view;
	int		size[4];		// framebuffer width and height, the pyramid's level 0's
	int		count;			// the levels to make: 1 or 2
	int		unused[3];
} vkPreparePush_t;

typedef struct {
	int		size[2];		// of the level the dispatch reads
	int		count;			// the levels it makes
	int		unused;
} vkHizPush_t;

typedef struct {
	vkTemporalView_t	view;
	int		mode;			// r_temporalShow
	int		mip;
	float	scale;
	float	sharpen;		// r_taaSharpen when it shows the anti-aliased scene
	int		rect[4];		// the scene's views (vkt.rect)
	float	hdr[8];			// HDR output's expansion (VK_OutputExpansion)
	int		mb[4];			// the motion blur's tile size, its pairs of samples, 1: the color is the depth of field's (its alpha: sharpen), 0
} vkCompositePush_t;

typedef struct {
	vkTemporalView_t	view;
	int		rect[4];		// the scene's views (vkt.rect)
	float	params[4];		// r_taaFeedback, 1: the history is the frame before's, 1: so is the pyramid, 1: it makes level 0 of this frame's
	float	weights[12];	// of the 3 x 3 (9), for this frame's color at the pixel's center
} vkTaaPush_t;

// the ambient occlusion's passes (ao_common.glsl, Push)
typedef struct {
	vkTemporalView_t	view;
	float	ao[4];			// the radius, the falloff's share of it, the thin occluder compensation, r_aoStrength
	float	ao2[4];			// the multi-bounce albedo, 1: the history is the frame before's, the most frames blended, the blur's center weight
	float	fogColor[4];	// rgb, 1 when the view is fogged
	float	fogParams[4];	// the fog factor's scale and offset, 1: the draws' normals, 1: the last view distances are the frame before's
	int		ints[4];		// slices, steps per side, the noise's frame index, the blur pass
} vkAoPush_t;

// the motion blur's passes (motion_blur.comp, Push)
typedef struct {
	vkTemporalView_t	view;
	int		rect[4];		// the scene's views (vkt.rect)
	float	params[4];		// half the exposure (in frames of motion), r_motionBlurCamera, the tile size, 0
	int		tiles[4];		// the tiles across and down, 0, 0
} vkMotionBlurPush_t;

// the depth of field's passes (dof.comp, Push)
typedef struct {
	vkTemporalView_t	view;
	int		rect[4];		// the scene's views (vkt.rect)
	float	lens[4];		// the blur's scale (pixels times units), the largest blur (the tile's side), 1 / r_dofFocus (0: autofocus), the focus's step
	float	params[4];		// the focus area's half side in pixels, 0, 1: r_temporalShow 13, 0
	int		ints[4];		// the tiles across and down, the gather's samples, 0
} vkDofPush_t;

#define VK_TEMPORAL_PREPARE_BINDINGS	4	// depth, normals (sampled), levels 0 and 1 (storage)
#define VK_TEMPORAL_COMPOSITE_BINDINGS	11
#define VK_TEMPORAL_TAA_BINDINGS		8	// color, motion vectors, normals, depth, history, last pyramid (sampled), result, this pyramid's level 0 (storage)
#define VK_TEMPORAL_TAA_HISTORY_BINDING	4	// read with the bilinear sampler
#define VK_TEMPORAL_TAA_GROUP			16	// the workgroup's width and height (taa_resolve.comp, GROUP)
#define VK_TEMPORAL_AO_BINDINGS		18	// ao_common.glsl: 9 sampled, 9 storage
#define VK_TEMPORAL_AO_SAMPLED			9
#define VK_TEMPORAL_MB_BINDINGS		6	// depth, normals, motion vectors (sampled), mbVelocity, mbTile, mbNeighbor (storage)
#define VK_TEMPORAL_MB_SAMPLED			3
#define VK_TEMPORAL_MB_NEIGHBOR_GROUP	8	// the neighbour pass's workgroup's width and height (motion_blur.comp)
#define VK_TEMPORAL_DOF_BINDINGS		10	// depth, normals, color (sampled), focus, half, tile, neighbor, far, near, result (storage)
#define VK_TEMPORAL_DOF_SAMPLED		3
#define VK_TEMPORAL_DOF_GROUP			8	// the workgroups' width and height (dof.comp)
// the depth of field's lens: r_dofAperture is the blur of the far distance
// when the focus is this far (128 units, about 3 m), and the projection's
// [5] of the game's default field of view (fov 80 at 4:3; a narrower field,
// a scope's zoom, blurs more, by its zoom)
#define VK_DOF_REFERENCE_FOCUS		128.0f
#define VK_DOF_PROJ_FOV80			1.5890f

// pairs of samples of the motion blur's gather at r_motionBlurQuality 0, 1, 2
static const int vk_mbPairs[3] = { 4, 6, 8 };
// samples of each of the depth of field's gathers at r_dofQuality 0, 1, 2
static const int vk_dofSamples[3] = { 16, 32, 48 };

void VK_TemporalRegisterCvars( void ) {
	r_temporal = ri.Cvar_Get( "r_temporal", "0", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_SetDescription( r_temporal, "1: the 3D view is drawn with motion vectors, normals, a depth pyramid and the last frame's color and depth (for temporal anti-aliasing and ambient occlusion; see the renderer's README)" );
	r_temporalJitter = ri.Cvar_Get( "r_temporalJitter", "0", 0 );
	ri.Cvar_SetDescription( r_temporalJitter, "with r_temporal 1: the 3D view's projection moves by a sub-pixel offset every frame (Halton 2, 3)" );
	r_temporalJitterSamples = ri.Cvar_Get( "r_temporalJitterSamples", "8", 0 );
	ri.Cvar_CheckRange( r_temporalJitterSamples, 1, 64, qtrue );
	r_temporalShow = ri.Cvar_Get( "r_temporalShow", "0", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_temporalShow, "with r_temporal 1, instead of the 3D view: 1 motion vectors, 2 normals, 3 classes, 4 depth pyramid level r_temporalShowMip, 5 last frame's color, 6 view distances, 7 last frame's view distances, 8 the scene without anti-aliasing, 9 the ambient occlusion, 10 one frame's ambient occlusion before denoising, 11 the motion blur of each pixel, 12 the longest motion blur around each tile, 13 the depth of field's blur (red in front of the focus, blue behind it, the focus area green)" );
	ri.Cvar_CheckRange( r_temporalShow, 0, 13, qtrue );
	r_temporalShowMip = ri.Cvar_Get( "r_temporalShowMip", "0", CVAR_CHEAT );
	ri.Cvar_CheckRange( r_temporalShowMip, 0, VK_TEMPORAL_MAX_MIPS - 1, qtrue );
	r_temporalShowScale = ri.Cvar_Get( "r_temporalShowScale", "20", CVAR_CHEAT );
	r_taa = ri.Cvar_Get( "r_taa", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_taa, "with r_temporal 1: temporal anti-aliasing (the projection is jittered with it)" );
	r_taaFeedback = ri.Cvar_Get( "r_taaFeedback", "0.75", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_taaFeedback, "with r_taa 1: the weight of the last frames in the result; more is smoother and blurs more in motion" );
	ri.Cvar_CheckRange( r_taaFeedback, 0.5f, 0.98f, qfalse );
	r_taaSharpen = ri.Cvar_Get( "r_taaSharpen", "0.8", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_taaSharpen, "with r_taa 1: sharpens the anti-aliased image, 0 off to 1" );
	ri.Cvar_CheckRange( r_taaSharpen, 0.0f, 1.0f, qfalse );
	r_saturation = ri.Cvar_Get( "r_saturation", "1.1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_saturation, "with r_temporal 1: the 3D view's color saturation (not the HUD's), 1 unchanged; in SDR and with HDR output" );
	ri.Cvar_CheckRange( r_saturation, 0.0f, 2.0f, qfalse );
	r_ao = ri.Cvar_Get( "r_ao", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_ao, "with r_temporal 1: ambient occlusion (ground truth ambient occlusion, GTAO), the contact shadows of corners, creases and what stands on a floor" );
	r_aoQuality = ri.Cvar_Get( "r_aoQuality", "2", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoQuality, "with r_ao 1: 0 low, 1 medium, 2 high, 3 ultra (directions and samples per pixel)" );
	ri.Cvar_CheckRange( r_aoQuality, 0, 3, qtrue );
	r_aoRadius = ri.Cvar_Get( "r_aoRadius", "48", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoRadius, "with r_ao 1: how far occluders reach, in world units" );
	ri.Cvar_CheckRange( r_aoRadius, 4.0f, 256.0f, qfalse );
	r_aoStrength = ri.Cvar_Get( "r_aoStrength", "1.5", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoStrength, "with r_ao 1: the occlusion's strength (a power of the visibility), 1 as computed" );
	ri.Cvar_CheckRange( r_aoStrength, 0.0f, 4.0f, qfalse );
	r_aoFalloff = ri.Cvar_Get( "r_aoFalloff", "0.6", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoFalloff, "with r_ao 1: the share of the radius over which occluders fade out" );
	ri.Cvar_CheckRange( r_aoFalloff, 0.05f, 1.0f, qfalse );
	r_aoThickness = ri.Cvar_Get( "r_aoThickness", "0.25", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoThickness, "with r_ao 1: the thin occluder compensation: occluders in front along the view fade sooner (less dark halos behind thin objects)" );
	ri.Cvar_CheckRange( r_aoThickness, 0.0f, 4.0f, qfalse );
	r_aoTemporal = ri.Cvar_Get( "r_aoTemporal", "16", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoTemporal, "with r_ao 1: the most frames blended (less noise, slower to react); 1: none" );
	ri.Cvar_CheckRange( r_aoTemporal, 1, 64, qtrue );
	r_aoNormals = ri.Cvar_Get( "r_aoNormals", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoNormals, "with r_ao 1: 0 the normals of the depth, 1 the draws' normals where they are close to those (smoother curved models)" );
	ri.Cvar_CheckRange( r_aoNormals, 0, 1, qtrue );
	r_aoMultiBounce = ri.Cvar_Get( "r_aoMultiBounce", "0.4", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_aoMultiBounce, "with r_ao 1: the albedo of the light bounced back by occluders (lightens the occlusion), 0 none" );
	ri.Cvar_CheckRange( r_aoMultiBounce, 0.0f, 0.9f, qfalse );
	r_motionBlur = ri.Cvar_Get( "r_motionBlur", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_motionBlur, "with r_temporal 1: motion blur of the camera's motion and of what moves, from the motion vectors and the depth" );
	r_motionBlurShutter = ri.Cvar_Get( "r_motionBlurShutter", "0.5", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_motionBlurShutter, "with r_motionBlur 1: the exposure, in frames at r_motionBlurFps (0.5: half a frame, 1/120 s at 60)" );
	ri.Cvar_CheckRange( r_motionBlurShutter, 0.0f, 2.0f, qfalse );
	r_motionBlurFps = ri.Cvar_Get( "r_motionBlurFps", "60", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_motionBlurFps, "with r_motionBlur 1: the frame rate whose frames r_motionBlurShutter counts, so the blur is the same at any frame rate; 0: the frames as they come (shorter blur the higher the frame rate)" );
	ri.Cvar_CheckRange( r_motionBlurFps, 0.0f, 1000.0f, qfalse );
	r_motionBlurCamera = ri.Cvar_Get( "r_motionBlurCamera", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_motionBlurCamera, "with r_motionBlur 1: the share of the camera's motion (turning, moving) in the blur; 0: only what moves" );
	ri.Cvar_CheckRange( r_motionBlurCamera, 0.0f, 1.0f, qfalse );
	r_motionBlurQuality = ri.Cvar_Get( "r_motionBlurQuality", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_motionBlurQuality, "with r_motionBlur 1: 0 low, 1 medium, 2 high (8, 12, 16 samples per pixel)" );
	r_dof = ri.Cvar_Get( "r_dof", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dof, "with r_temporal 1: depth of field, focused on what is under the crosshair (or at r_dofFocus); the view weapon stays sharp" );
	r_dofAperture = ri.Cvar_Get( "r_dofAperture", "0.5", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dofAperture, "with r_dof 1: the lens' aperture: the blur of the far distance when focused 128 units (3 m) away, in percent of the view's height; a scope's zoom blurs more" );
	ri.Cvar_CheckRange( r_dofAperture, 0.0f, 5.0f, qfalse );
	r_dofFocus = ri.Cvar_Get( "r_dofFocus", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dofFocus, "with r_dof 1: 0 focus on what is under the crosshair; more: a fixed focus distance in units (screenshots)" );
	ri.Cvar_CheckRange( r_dofFocus, 0.0f, 65536.0f, qfalse );
	r_dofFocusSpeed = ri.Cvar_Get( "r_dofFocusSpeed", "0.25", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dofFocusSpeed, "with r_dof 1: how long the focus takes to follow, in seconds (the time constant of its easing); 0 at once" );
	ri.Cvar_CheckRange( r_dofFocusSpeed, 0.0f, 10.0f, qfalse );
	r_dofQuality = ri.Cvar_Get( "r_dofQuality", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dofQuality, "with r_dof 1: 0 low, 1 medium, 2 high (16, 32, 48 samples per gather where something is blurred)" );
	ri.Cvar_CheckRange( r_dofQuality, 0, 2, qtrue );
	ri.Cvar_CheckRange( r_motionBlurQuality, 0, 2, qtrue );
}

/*
===============
VK_TemporalWritesBuffers

Whether a pipeline of the scene writes the normals buffer (and with
VK_PIPE_MOTION the motion vectors): it writes depth (and color) without
blending, so it hides what is behind it. The others mask them and skip their
work (specialization constants 11 and 12 of the shaders)
===============
*/
qboolean VK_TemporalWritesBuffers( uint64_t bits ) {
	const uint64_t blend = ( 15ull << VK_PIPE_SRCBLEND_SHIFT ) | ( 15ull << VK_PIPE_DSTBLEND_SHIFT );

	return ( bits & VK_PIPE_DEPTHWRITE ) && !( bits & blend ) && ( bits & ( 7ull << VK_PIPE_COLORMASK_SHIFT ) );
}

/*
====================================================================

TARGETS

====================================================================
*/

/*
===============
VK_TemporalCheckDevice

Whether the temporal path is on: r_temporal, no multisampling, and what the
device must have. Called by VK_CreateRenderTargets once the sample count
and the depth format are chosen, before the depth image is created (it
must be sampled too)
===============
*/
void VK_TemporalCheckDevice( void ) {
	const VkFormatFeatureFlags attachment = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	const uint32_t pushSize = Q_max( Q_max( Q_max( Q_max( Q_max( VK_TEMPORAL_PUSH_END, (uint32_t)sizeof( vkPreparePush_t ) ), (uint32_t)sizeof( vkCompositePush_t ) ),
		(uint32_t)sizeof( vkTaaPush_t ) ), (uint32_t)sizeof( vkAoPush_t ) ), (uint32_t)Q_max( sizeof( vkMotionBlurPush_t ), sizeof( vkDofPush_t ) ) );
	const VkFormatFeatureFlags storage = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	const char *missing = NULL;

	Com_Memset( &vkt, 0, sizeof( vkt ) );
	if ( !r_temporal || !r_temporal->integer ) {
		return;
	}

	// the scene's color in RGBA16F: room above 1 for HDR to come (the
	// frame's 8-bit format was the same frames as without the path and 0.03
	// ms less at 2560x1440, but clips at 1; see the README)
	vkt.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	vkt.velocityFormat = VK_FORMAT_R16G16_SFLOAT;
	vkt.normalFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	vkt.hizFormat = VK_FORMAT_R32G32_SFLOAT;
	// the anti-aliasing's results in RGBA16F, as the scene (A2B10G10R10 was
	// 0.025 ms less, with a little more shimmer and no room above 1)
	vkt.taaFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

	if ( vk.samples != VK_SAMPLE_COUNT_1_BIT ) {
		missing = "multisampling (r_ext_multisample) is on";
	} else if ( vk.deviceProperties.limits.maxPushConstantsSize < pushSize ) {
		missing = va( "the device has %u bytes of push constants, %u are needed", vk.deviceProperties.limits.maxPushConstantsSize, pushSize );
	} else if ( !vk.enabledFeatures.shaderStorageImageExtendedFormats ) {
		missing = "the device has no shaderStorageImageExtendedFormats";
	} else if ( !vk.enabledFeatures.independentBlend ) {
		missing = "the device has no independentBlend";
	} else if ( !VK_FormatHasFeatures( vkt.colorFormat, attachment | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT ) ) {
		missing = "RGBA16F can't be the scene's color target";
	} else if ( !VK_FormatHasFeatures( vkt.velocityFormat, attachment ) ) {
		missing = "RG16F can't hold the motion vectors";
	} else if ( !VK_FormatHasFeatures( vkt.normalFormat, attachment ) ) {
		missing = "A2B10G10R10 can't hold the normals";
	} else if ( !VK_FormatHasFeatures( vkt.hizFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
		| VK_FORMAT_FEATURE_TRANSFER_DST_BIT ) ) {
		missing = "RG32F can't hold the depth pyramid";
	} else if ( !VK_FormatHasFeatures( vkt.taaFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
		| VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT ) ) {
		missing = "RGBA16F can't hold the anti-aliasing's history";
	} else if ( !VK_FormatHasFeatures( vk.depthFormat, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ) ) {
		missing = "the depth format can't be sampled";
	} else if ( !VK_FormatHasFeatures( VK_FORMAT_R32_SFLOAT, storage ) || !VK_FormatHasFeatures( VK_FORMAT_R8_UNORM, storage )
		|| !VK_FormatHasFeatures( VK_FORMAT_R16G16_SFLOAT, storage ) || !VK_FormatHasFeatures( VK_FORMAT_R8G8B8A8_UNORM, storage ) ) {
		missing = "R32F, R8, RG16F and RGBA8 can't hold the ambient occlusion";
	}

	if ( missing ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "r_temporal: off, %s\n", missing );
		return;
	}
	vkt.enabled = qtrue;
}

static VkImageView VK_TemporalLevelView( VkImage image, VkFormat format, VkImageAspectFlags aspect, int level ) {
	VkImageViewCreateInfo viewInfo;
	VkImageView view;

	Com_Memset( &viewInfo, 0, sizeof( viewInfo ) );
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspect;
	viewInfo.subresourceRange.baseMipLevel = level;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CHECK( qvkCreateImageView( vk.device, &viewInfo, NULL, &view ) );
	return view;
}

static void VK_TemporalCreateImage( vkTemporalImage_t *img, VkFormat format, VkImageUsageFlags usage, int width, int height, int mipLevels ) {
	VK_CreateImage( format, VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_ASPECT_COLOR_BIT, width, height, mipLevels,
		&img->image, &img->memory, &img->view );
}

static void VK_TemporalDestroyImage( vkTemporalImage_t *img ) {
	VK_DestroyTargetImage( &img->image, &img->memory, &img->view );
}

/*
===============
VK_TemporalCreateRenderPass

The scene's: color, motion vectors, normals, depth/stencil. The one that
begins a scene (clear) clears the color and the normals, and doesn't load
the motion vectors (only where the normals' flag says a draw wrote them are
they read); what they held doesn't matter, and the passes after the scenes
before read them: a dependency on those stages. The one that continues it
keeps everything, like vk.renderPass. They are compatible (they differ in
load operations and initial layouts only): the same pipelines and
framebuffers serve both
===============
*/
static VkRenderPass VK_TemporalCreateRenderPass( qboolean clear ) {
	VkAttachmentDescription attachments[4];
	VkAttachmentReference colorRefs[3], depthRef;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo info;
	VkRenderPass renderPass;
	VkPipelineStageFlags stages;
	VkAccessFlags access;
	int i;

	Com_Memset( attachments, 0, sizeof( attachments ) );
	attachments[0].format = vkt.colorFormat;
	attachments[1].format = vkt.velocityFormat;
	attachments[2].format = vkt.normalFormat;
	attachments[3].format = vk.depthFormat;
	for ( i = 0; i < 4; i++ ) {
		qboolean depth = i == 3;

		attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		if ( clear && !depth ) {
			attachments[i].loadOp = i == 1 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
		}
		attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[i].stencilLoadOp = depth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[i].stencilStoreOp = depth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[i].finalLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[i].initialLayout = clear && !depth ? VK_IMAGE_LAYOUT_UNDEFINED : attachments[i].finalLayout;
	}
	for ( i = 0; i < 3; i++ ) {
		colorRefs[i].attachment = i;
		colorRefs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	depthRef.attachment = 3;
	depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	Com_Memset( &subpass, 0, sizeof( subpass ) );
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 3;
	subpass.pColorAttachments = colorRefs;
	subpass.pDepthStencilAttachment = &depthRef;

	// as vk.renderPass: successive render passes read what the ones before wrote
	stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	Com_Memset( dependencies, 0, sizeof( dependencies ) );
	for ( i = 0; i < 2; i++ ) {
		dependencies[i].srcSubpass = i ? 0 : VK_SUBPASS_EXTERNAL;
		dependencies[i].dstSubpass = i ? VK_SUBPASS_EXTERNAL : 0;
		dependencies[i].srcStageMask = stages;
		dependencies[i].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[i].dstStageMask = stages;
		dependencies[i].dstAccessMask = access;
	}
	// the images were last read by the passes after the scenes before (the
	// clear's write waits for them); the same in both render passes, which
	// are only compatible with identical dependencies
	dependencies[0].srcStageMask |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = ARRAY_LEN( attachments );
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = ARRAY_LEN( dependencies );
	info.pDependencies = dependencies;
	VK_CHECK( qvkCreateRenderPass( vk.device, &info, NULL, &renderPass ) );
	return renderPass;
}

// the framebuffers and the sampled view of vk.depthImage, which
// VK_UpdateDepthTarget replaces
static void VK_TemporalCreateDepthObjects( void ) {
	VkFramebufferCreateInfo info;
	VkImageView views[4];
	int i;

	vkt.depthView = VK_TemporalLevelView( vk.depthImage, vk.depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 0 );

	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		// one color image, or two that alternate when the last scene's is kept
		views[0] = vkt.color[( vkt.groups & VK_TEMPORAL_GROUP_PREVCOLOR ) ? i : 0].view;
		views[1] = vkt.velocity.view;
		views[2] = vkt.normal.view;
		views[3] = vk.depthView;
		Com_Memset( &info, 0, sizeof( info ) );
		info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		info.renderPass = vkt.renderPass;
		info.attachmentCount = 4;
		info.pAttachments = views;
		info.width = vk.width;
		info.height = vk.height;
		info.layers = 1;
		VK_CHECK( qvkCreateFramebuffer( vk.device, &info, NULL, &vkt.framebuffers[i] ) );
	}
}

void VK_TemporalDepthChanged( void ) {
	int i;

	if ( !vkt.enabled || !vkt.renderPass ) {
		return;
	}
	// frames in flight may still use them
	VK_DeferDestroy( VK_DESTROY_IMAGE_VIEW, (uint64_t)vkt.depthView, 0 );
	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		VK_DeferDestroy( VK_DESTROY_FRAMEBUFFER, (uint64_t)vkt.framebuffers[i], 0 );
	}
	VK_TemporalCreateDepthObjects();
}

// clears an image in the frame being recorded and leaves it in layout
static void VK_TemporalClearImage( VkCommandBuffer cmd, VkImage image, VkImageLayout layout, const float *color ) {
	VkImageSubresourceRange range;
	VkClearColorValue value;

	Com_Memset( &range, 0, sizeof( range ) );
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = VK_REMAINING_MIP_LEVELS;
	range.layerCount = 1;
	Com_Memcpy( value.float32, color, sizeof( value.float32 ) );

	VK_ImageBarrier( cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
	qvkCmdClearColorImage( cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range );
	VK_ImageBarrier( cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT );
}

/*
===============
VK_TemporalPlanPyramid

The dispatches of the pyramid after the prepare pass (levels 0 and 1): two
levels each, as long as the level they read is larger than
VK_TEMPORAL_TAIL_SIZE, then the rest in one (at most
VK_TEMPORAL_TAIL_IMAGES - 1 levels: 64, 32, ... 1)
===============
*/
static void VK_TemporalPlanPyramid( void ) {
	int level = Q_min( 1, vkt.hizLevels - 1 );

	vkt.numHizPasses = 0;
	while ( level < vkt.hizLevels - 1 && vkt.numHizPasses < VK_TEMPORAL_MAX_PASSES ) {
		vkHizPass_t *pass = &vkt.hizPasses[vkt.numHizPasses++];
		int w = Q_max( vkt.hizWidth >> level, 1 );
		int h = Q_max( vkt.hizHeight >> level, 1 );

		pass->src = level;
		if ( w <= VK_TEMPORAL_TAIL_SIZE && h <= VK_TEMPORAL_TAIL_SIZE ) {
			pass->tail = qtrue;
			pass->count = vkt.hizLevels - 1 - level;
		} else {
			pass->tail = qfalse;
			pass->count = Q_min( 2, vkt.hizLevels - 1 - level );
		}
		level += pass->count;
	}
}

// the images of the groups (VK_TEMPORAL_GROUP_*), recorded in cmd: they
// start cleared (black, no history: their serials are 0)
static void VK_TemporalCreateGroup( int group, VkCommandBuffer cmd ) {
	static const float black[4] = { 0, 0, 0, 1 };
	static const float zero[4] = { 0, 0, 0, 0 };
	static const float aoClear[4] = { 0, 0, 0, 1 };	// the ambient occlusion's result: nothing added, factor 1
	const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	int i, l;

	switch ( group ) {
	case VK_TEMPORAL_GROUP_TAA:
		for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
			VK_TemporalCreateImage( &vkt.taa[i], vkt.taaFormat, storage, vk.width, vk.height, 1 );
			VK_TemporalClearImage( cmd, vkt.taa[i].image, VK_IMAGE_LAYOUT_GENERAL, black );
		}
		Com_Memset( vkt.taaSerial, 0, sizeof( vkt.taaSerial ) );
		break;
	case VK_TEMPORAL_GROUP_PYRAMID:
		for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
			VK_TemporalCreateImage( &vkt.hiz[i], vkt.hizFormat, storage, vkt.hizWidth, vkt.hizHeight, vkt.hizLevels );
			for ( l = 0; l < vkt.hizLevels; l++ ) {
				vkt.hizLevel[i][l] = VK_TemporalLevelView( vkt.hiz[i].image, vkt.hizFormat, VK_IMAGE_ASPECT_COLOR_BIT, l );
			}
			VK_TemporalClearImage( cmd, vkt.hiz[i].image, VK_IMAGE_LAYOUT_GENERAL, zero );
		}
		Com_Memset( vkt.hizSerial, 0, sizeof( vkt.hizSerial ) );
		break;
	case VK_TEMPORAL_GROUP_AO:
		for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
			VK_TemporalCreateImage( &vkt.aoDepth[i], VK_FORMAT_R32_SFLOAT, storage, vk.width, vk.height, VK_TEMPORAL_AO_LEVELS );
			for ( l = 0; l < VK_TEMPORAL_AO_LEVELS; l++ ) {
				vkt.aoDepthLevel[i][l] = VK_TemporalLevelView( vkt.aoDepth[i].image, VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, l );
			}
			VK_TemporalCreateImage( &vkt.aoHistory[i], VK_FORMAT_R16G16_SFLOAT, storage, vk.width, vk.height, 1 );
			VK_TemporalClearImage( cmd, vkt.aoDepth[i].image, VK_IMAGE_LAYOUT_GENERAL, zero );
			VK_TemporalClearImage( cmd, vkt.aoHistory[i].image, VK_IMAGE_LAYOUT_GENERAL, zero );
		}
		VK_TemporalCreateImage( &vkt.aoRaw, VK_FORMAT_R8_UNORM, storage, vk.width, vk.height, 1 );
		VK_TemporalCreateImage( &vkt.aoBlur, VK_FORMAT_R16G16_SFLOAT, storage, vk.width, vk.height, 1 );
		VK_TemporalCreateImage( &vkt.aoResult, VK_FORMAT_R8G8B8A8_UNORM, storage, vk.width, vk.height, 1 );
		VK_TemporalClearImage( cmd, vkt.aoRaw.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.aoBlur.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.aoResult.image, VK_IMAGE_LAYOUT_GENERAL, aoClear );
		Com_Memset( vkt.aoSerial, 0, sizeof( vkt.aoSerial ) );
		Com_Memset( vkt.aoDepthSerial, 0, sizeof( vkt.aoDepthSerial ) );
		break;
	case VK_TEMPORAL_GROUP_PREVCOLOR:
		VK_TemporalCreateImage( &vkt.color[1], vkt.colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
			| VK_IMAGE_USAGE_TRANSFER_DST_BIT, vk.width, vk.height, 1 );
		VK_TemporalClearImage( cmd, vkt.color[1].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, black );
		break;
	case VK_TEMPORAL_GROUP_MOTIONBLUR:
		VK_TemporalCreateImage( &vkt.mbVelocity, VK_FORMAT_R16G16B16A16_SFLOAT, storage, vk.width, vk.height, 1 );
		VK_TemporalCreateImage( &vkt.mbTile, VK_FORMAT_R16G16_SFLOAT, storage, vkt.mbTilesX, vkt.mbTilesY, 1 );
		VK_TemporalCreateImage( &vkt.mbNeighbor, VK_FORMAT_R16G16_SFLOAT, storage, vkt.mbTilesX, vkt.mbTilesY, 1 );
		VK_TemporalClearImage( cmd, vkt.mbVelocity.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.mbTile.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.mbNeighbor.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		break;
	case VK_TEMPORAL_GROUP_DOF:
		// the focus 0: the first scene takes its target at once
		VK_TemporalCreateImage( &vkt.dofFocus, VK_FORMAT_R32_SFLOAT, storage, 1, 1, 1 );
		VK_TemporalCreateImage( &vkt.dofHalf, VK_FORMAT_R16G16B16A16_SFLOAT, storage, vkt.dofHalfWidth, vkt.dofHalfHeight, 1 );
		VK_TemporalCreateImage( &vkt.dofTile, VK_FORMAT_R16G16_SFLOAT, storage, vkt.dofTilesX, vkt.dofTilesY, 1 );
		VK_TemporalCreateImage( &vkt.dofNeighbor, VK_FORMAT_R16G16_SFLOAT, storage, vkt.dofTilesX, vkt.dofTilesY, 1 );
		VK_TemporalCreateImage( &vkt.dofFar, VK_FORMAT_R16G16B16A16_SFLOAT, storage, vkt.dofHalfWidth, vkt.dofHalfHeight, 1 );
		VK_TemporalCreateImage( &vkt.dofNear, VK_FORMAT_R16G16B16A16_SFLOAT, storage, vkt.dofHalfWidth, vkt.dofHalfHeight, 1 );
		VK_TemporalCreateImage( &vkt.dofResult, VK_FORMAT_R16G16B16A16_SFLOAT, storage, vk.width, vk.height, 1 );
		VK_TemporalClearImage( cmd, vkt.dofFocus.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.dofHalf.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.dofTile.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.dofNeighbor.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.dofFar.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.dofNear.image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.dofResult.image, VK_IMAGE_LAYOUT_GENERAL, black );
		break;
	}
	vkt.groups |= group;
}

// when nothing uses them anymore (the GPU idle)
static void VK_TemporalDestroyGroup( int group ) {
	int i, l;

	switch ( group ) {
	case VK_TEMPORAL_GROUP_TAA:
		for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
			VK_TemporalDestroyImage( &vkt.taa[i] );
		}
		break;
	case VK_TEMPORAL_GROUP_PYRAMID:
		for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
			for ( l = 0; l < VK_TEMPORAL_MAX_MIPS; l++ ) {
				if ( vkt.hizLevel[i][l] ) {
					qvkDestroyImageView( vk.device, vkt.hizLevel[i][l], NULL );
					vkt.hizLevel[i][l] = VK_NULL_HANDLE;
				}
			}
			VK_TemporalDestroyImage( &vkt.hiz[i] );
		}
		break;
	case VK_TEMPORAL_GROUP_AO:
		for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
			for ( l = 0; l < VK_TEMPORAL_AO_LEVELS; l++ ) {
				if ( vkt.aoDepthLevel[i][l] ) {
					qvkDestroyImageView( vk.device, vkt.aoDepthLevel[i][l], NULL );
					vkt.aoDepthLevel[i][l] = VK_NULL_HANDLE;
				}
			}
			VK_TemporalDestroyImage( &vkt.aoDepth[i] );
			VK_TemporalDestroyImage( &vkt.aoHistory[i] );
		}
		VK_TemporalDestroyImage( &vkt.aoRaw );
		VK_TemporalDestroyImage( &vkt.aoBlur );
		VK_TemporalDestroyImage( &vkt.aoResult );
		break;
	case VK_TEMPORAL_GROUP_PREVCOLOR:
		VK_TemporalDestroyImage( &vkt.color[1] );
		break;
	case VK_TEMPORAL_GROUP_MOTIONBLUR:
		VK_TemporalDestroyImage( &vkt.mbVelocity );
		VK_TemporalDestroyImage( &vkt.mbTile );
		VK_TemporalDestroyImage( &vkt.mbNeighbor );
		break;
	case VK_TEMPORAL_GROUP_DOF:
		VK_TemporalDestroyImage( &vkt.dofFocus );
		VK_TemporalDestroyImage( &vkt.dofHalf );
		VK_TemporalDestroyImage( &vkt.dofTile );
		VK_TemporalDestroyImage( &vkt.dofNeighbor );
		VK_TemporalDestroyImage( &vkt.dofFar );
		VK_TemporalDestroyImage( &vkt.dofNear );
		VK_TemporalDestroyImage( &vkt.dofResult );
		break;
	}
	vkt.groups &= ~group;
}

/*
===============
VK_TemporalCreateTargets

The scene's images (the color, the motion vectors, the normals), render
passes and framebuffers; at the end of VK_CreateRenderTargets, in its frame.
The readers' own images are made when one of them is on
(VK_TemporalUpdateFeatures). The color starts black; the motion vectors and
the normals are written by every scene before anything reads them
===============
*/
void VK_TemporalCreateTargets( void ) {
	static const float black[4] = { 0, 0, 0, 1 };
	static const float zero[4] = { 0, 0, 0, 0 };
	const VkImageUsageFlags attachment = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkSamplerCreateInfo samplerInfo;
	VkCommandBuffer cmd;
	int size;

	if ( !vkt.enabled ) {
		return;
	}

	// level 0 of the pyramid covers 2 x 2 pixels
	vkt.hizWidth = Q_max( vk.width >> 1, 1 );
	vkt.hizHeight = Q_max( vk.height >> 1, 1 );
	for ( size = Q_max( vkt.hizWidth, vkt.hizHeight ), vkt.hizLevels = 1; size > 1 && vkt.hizLevels < VK_TEMPORAL_MAX_MIPS; size >>= 1 ) {
		vkt.hizLevels++;
	}
	VK_TemporalPlanPyramid();

	// the motion blur's tiles: their side is the longest half streak, a
	// share of the height, so the blur looks the same at any resolution (27
	// pixels at 1080, 36 at 1440, 54 at 2160)
	vkt.mbTileSize = Q_max( 16, ( vk.height + 20 ) / 40 );
	vkt.mbTilesX = ( vk.width + vkt.mbTileSize - 1 ) / vkt.mbTileSize;
	vkt.mbTilesY = ( vk.height + vkt.mbTileSize - 1 ) / vkt.mbTileSize;

	// the depth of field's: their side is the largest blur, even (a tile is
	// half of it across at half the resolution): 54 pixels at 1080, 72 at
	// 1440, 108 at 2160
	vkt.dofTileSize = Q_max( 16, ( vk.height + 10 ) / 20 ) & ~1;
	vkt.dofHalfWidth = ( vk.width + 1 ) / 2;
	vkt.dofHalfHeight = ( vk.height + 1 ) / 2;
	vkt.dofTilesX = ( vkt.dofHalfWidth + vkt.dofTileSize / 2 - 1 ) / ( vkt.dofTileSize / 2 );
	vkt.dofTilesY = ( vkt.dofHalfHeight + vkt.dofTileSize / 2 - 1 ) / ( vkt.dofTileSize / 2 );

	// the images every scene draws; the readers' own come with them
	// (VK_TemporalUpdateFeatures)
	VK_TemporalCreateImage( &vkt.color[0], vkt.colorFormat, attachment | VK_IMAGE_USAGE_TRANSFER_DST_BIT, vk.width, vk.height, 1 );
	VK_TemporalCreateImage( &vkt.velocity, vkt.velocityFormat, attachment, vk.width, vk.height, 1 );
	VK_TemporalCreateImage( &vkt.normal, vkt.normalFormat, attachment, vk.width, vk.height, 1 );
	VK_TemporalCreateImage( &vkt.placeholder, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, 1, 1 );
	vkt.groups = 0;

	vkt.renderPass = VK_TemporalCreateRenderPass( qfalse );
	vkt.renderPassClear = VK_TemporalCreateRenderPass( qtrue );
	VK_TemporalCreateDepthObjects();

	Com_Memset( &samplerInfo, 0, sizeof( samplerInfo ) );
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
	VK_CHECK( qvkCreateSampler( vk.device, &samplerInfo, NULL, &vkt.sampler ) );
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.maxLod = 0.0f;
	VK_CHECK( qvkCreateSampler( vk.device, &samplerInfo, NULL, &vkt.linearSampler ) );

	// the normals of the scene's draws that have none, read with a stride of 0
	VK_CreateBuffer( &vkt.zeroBuffer, sizeof( float ) * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 );
	Com_Memset( vkt.zeroBuffer.mapped, 0, sizeof( float ) * 4 );

	// the layouts between scenes
	VK_BeginFrameIfNeeded();
	cmd = vk.frame->mainCmd;
	VK_TemporalClearImage( cmd, vkt.color[0].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, black );
	VK_TemporalClearImage( cmd, vkt.placeholder.image, VK_IMAGE_LAYOUT_GENERAL, zero );

	vkt.current = 0;
	vkt.colorIndex = 0;
	vkt.sceneSerial = 0;
	vkt.historySerial = 0;

	ri.Printf( PRINT_ALL, "Vulkan temporal path (r_temporal): %dx%d, depth pyramid of %d levels from %dx%d in %d dispatches\n",
		vk.width, vk.height, vkt.hizLevels, vkt.hizWidth, vkt.hizHeight, vkt.numHizPasses + 1 );
}

// called when nothing uses them anymore
void VK_TemporalDestroyTargets( void ) {
	int i;

	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		if ( vkt.framebuffers[i] ) {
			qvkDestroyFramebuffer( vk.device, vkt.framebuffers[i], NULL );
			vkt.framebuffers[i] = VK_NULL_HANDLE;
		}
	}
	for ( i = 1; i <= VK_TEMPORAL_GROUP_LAST; i <<= 1 ) {
		VK_TemporalDestroyGroup( i );
	}
	VK_TemporalDestroyImage( &vkt.color[0] );
	VK_TemporalDestroyImage( &vkt.placeholder );
	VK_TemporalDestroyImage( &vkt.velocity );
	VK_TemporalDestroyImage( &vkt.normal );
	if ( vkt.depthView ) {
		qvkDestroyImageView( vk.device, vkt.depthView, NULL );
		vkt.depthView = VK_NULL_HANDLE;
	}
	if ( vkt.renderPass ) {
		qvkDestroyRenderPass( vk.device, vkt.renderPass, NULL );
		vkt.renderPass = VK_NULL_HANDLE;
	}
	if ( vkt.renderPassClear ) {
		qvkDestroyRenderPass( vk.device, vkt.renderPassClear, NULL );
		vkt.renderPassClear = VK_NULL_HANDLE;
	}
	if ( vkt.sampler ) {
		qvkDestroySampler( vk.device, vkt.sampler, NULL );
		vkt.sampler = VK_NULL_HANDLE;
	}
	if ( vkt.linearSampler ) {
		qvkDestroySampler( vk.device, vkt.linearSampler, NULL );
		vkt.linearSampler = VK_NULL_HANDLE;
	}
	VK_DestroyBuffer( &vkt.zeroBuffer );
	vkt.active = qfalse;
}

/*
====================================================================

PIPELINES

====================================================================
*/

// count bindings of one descriptor each, of types[i]
static VkDescriptorSetLayout VK_TemporalSetLayout( const VkDescriptorType *types, int count, VkShaderStageFlags stages ) {
	VkDescriptorSetLayoutBinding bindings[VK_TEMPORAL_MAX_BINDINGS];
	VkDescriptorSetLayoutCreateInfo info;
	VkDescriptorSetLayout layout;
	int i;

	Com_Memset( bindings, 0, sizeof( bindings ) );
	for ( i = 0; i < count; i++ ) {
		bindings[i].binding = i;
		bindings[i].descriptorType = types[i];
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = stages;
	}
	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = count;
	info.pBindings = bindings;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &info, NULL, &layout ) );
	return layout;
}

static VkPipelineLayout VK_TemporalPipelineLayout( const VkDescriptorSetLayout *sets, int numSets, const VkPushConstantRange *ranges, int numRanges ) {
	VkPipelineLayoutCreateInfo info;
	VkPipelineLayout layout;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	info.setLayoutCount = numSets;
	info.pSetLayouts = sets;
	info.pushConstantRangeCount = numRanges;
	info.pPushConstantRanges = ranges;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &info, NULL, &layout ) );
	return layout;
}

// a pipeline layout of one set and one push constant range
static VkPipelineLayout VK_TemporalSimpleLayout( VkDescriptorSetLayout set, VkShaderStageFlags stages, uint32_t pushSize ) {
	VkPushConstantRange range;

	range.stageFlags = stages;
	range.offset = 0;
	range.size = pushSize;
	return VK_TemporalPipelineLayout( &set, 1, &range, 1 );
}

// with spec (NULL for none) the shader's specialization constants
static VkPipeline VK_TemporalComputePipelineSpec( VkShaderModule module, VkPipelineLayout layout, const VkSpecializationInfo *spec ) {
	VkComputePipelineCreateInfo info;
	VkPipeline pipeline;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.stage.pSpecializationInfo = spec;
	info.layout = layout;
	VK_CHECK( qvkCreateComputePipelines( vk.device, vk.pipelineCache, 1, &info, NULL, &pipeline ) );
	return pipeline;
}

static VkPipeline VK_TemporalComputePipeline( VkShaderModule module, VkPipelineLayout layout ) {
	return VK_TemporalComputePipelineSpec( module, layout, NULL );
}

// the ambient occlusion's main pass at r_aoQuality q: slices and steps a
// side (XeGTAO's low, medium, high and ultra) as specialization constants
static const int vk_aoQuality[4][2] = { { 1, 2 }, { 2, 2 }, { 3, 3 }, { 9, 3 } };

static VkPipeline VK_TemporalAoMainPipeline( int q ) {
	VkSpecializationMapEntry entries[2];
	VkSpecializationInfo spec;

	entries[0].constantID = 0;
	entries[0].offset = 0;
	entries[0].size = sizeof( int );
	entries[1].constantID = 1;
	entries[1].offset = sizeof( int );
	entries[1].size = sizeof( int );
	spec.mapEntryCount = 2;
	spec.pMapEntries = entries;
	spec.dataSize = sizeof( vk_aoQuality[q] );
	spec.pData = vk_aoQuality[q];
	return VK_TemporalComputePipelineSpec( vkt.aoMainShader, vkt.aoLayout, &spec );
}

// the scene into the frame's color target: a triangle over the framebuffer
// in vk.renderPass, no vertex input, no depth test, no blending; with
// motionBlur the motion blur's gather (MOTION_BLUR)
static VkPipeline VK_TemporalCompositePipeline( VkBool32 motionBlur ) {
	VkSpecializationMapEntry specEntries[2];
	VkSpecializationInfo specInfo;
	VkBool32 specData[2];
	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineVertexInputStateCreateInfo vertexInput;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly;
	VkPipelineViewportStateCreateInfo viewportState;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depthStencil;
	VkPipelineColorBlendAttachmentState blendAttachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState dynamicStates[2];
	VkPipelineDynamicStateCreateInfo dynamicState;
	VkGraphicsPipelineCreateInfo info;
	VkPipeline pipeline;

	Com_Memset( stages, 0, sizeof( stages ) );
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vkt.compositeVertexShader;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = vkt.compositeFragmentShader;
	stages[1].pName = "main";
	// HDR output: the scene expanded toward the display's peak
	specData[0] = vko.enabled ? VK_TRUE : VK_FALSE;
	specData[1] = motionBlur;
	specEntries[0].constantID = 0;
	specEntries[0].offset = 0;
	specEntries[0].size = sizeof( VkBool32 );
	specEntries[1].constantID = 1;
	specEntries[1].offset = sizeof( VkBool32 );
	specEntries[1].size = sizeof( VkBool32 );
	specInfo.mapEntryCount = 2;
	specInfo.pMapEntries = specEntries;
	specInfo.dataSize = sizeof( specData );
	specInfo.pData = specData;
	stages[1].pSpecializationInfo = &specInfo;

	Com_Memset( &vertexInput, 0, sizeof( vertexInput ) );
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	Com_Memset( &inputAssembly, 0, sizeof( inputAssembly ) );
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	Com_Memset( &viewportState, 0, sizeof( viewportState ) );
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;
	Com_Memset( &rasterization, 0, sizeof( rasterization ) );
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;
	Com_Memset( &multisample, 0, sizeof( multisample ) );
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	Com_Memset( &depthStencil, 0, sizeof( depthStencil ) );
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.maxDepthBounds = 1.0f;
	Com_Memset( &blendAttachment, 0, sizeof( blendAttachment ) );
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	Com_Memset( &blend, 0, sizeof( blend ) );
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttachment;
	dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
	Com_Memset( &dynamicState, 0, sizeof( dynamicState ) );
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &inputAssembly;
	info.pViewportState = &viewportState;
	info.pRasterizationState = &rasterization;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depthStencil;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamicState;
	info.layout = vkt.compositeLayout;
	info.renderPass = vk.renderPass;
	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, vk.pipelineCache, 1, &info, NULL, &pipeline ) );
	return pipeline;
}

/*
===============
VK_TemporalAoApplyPipeline

The ambient occlusion into the scene: a triangle over the framebuffer in the
scene's render pass (vkt.renderPass), no depth test; the color blended ONE,
SRC_ALPHA (dst a + rgb, ao_denoise.comp), its alpha kept, the motion vectors
and the normals not written
===============
*/
static VkPipeline VK_TemporalAoApplyPipeline( void ) {
	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineVertexInputStateCreateInfo vertexInput;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly;
	VkPipelineViewportStateCreateInfo viewportState;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depthStencil;
	VkPipelineColorBlendAttachmentState blendAttachments[3];
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState dynamicStates[2];
	VkPipelineDynamicStateCreateInfo dynamicState;
	VkGraphicsPipelineCreateInfo info;
	VkPipeline pipeline;

	Com_Memset( stages, 0, sizeof( stages ) );
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vkt.compositeVertexShader;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = vkt.aoApplyShader;
	stages[1].pName = "main";

	Com_Memset( &vertexInput, 0, sizeof( vertexInput ) );
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	Com_Memset( &inputAssembly, 0, sizeof( inputAssembly ) );
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	Com_Memset( &viewportState, 0, sizeof( viewportState ) );
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;
	Com_Memset( &rasterization, 0, sizeof( rasterization ) );
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;
	Com_Memset( &multisample, 0, sizeof( multisample ) );
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	Com_Memset( &depthStencil, 0, sizeof( depthStencil ) );
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.maxDepthBounds = 1.0f;
	Com_Memset( blendAttachments, 0, sizeof( blendAttachments ) );
	blendAttachments[0].blendEnable = VK_TRUE;
	blendAttachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachments[0].colorBlendOp = VK_BLEND_OP_ADD;
	blendAttachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	blendAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
	blendAttachments[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
	Com_Memset( &blend, 0, sizeof( blend ) );
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 3;
	blend.pAttachments = blendAttachments;
	dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
	Com_Memset( &dynamicState, 0, sizeof( dynamicState ) );
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &inputAssembly;
	info.pViewportState = &viewportState;
	info.pRasterizationState = &rasterization;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depthStencil;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamicState;
	info.layout = vkt.aoApplyLayout;
	info.renderPass = vkt.renderPass;
	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, vk.pipelineCache, 1, &info, NULL, &pipeline ) );
	return pipeline;
}

static void VK_TemporalAllocateSets( VkDescriptorSetLayout layout, VkDescriptorSet *sets, int count ) {
	VkDescriptorSetLayout layouts[VK_TEMPORAL_MAX_PASSES];
	VkDescriptorSetAllocateInfo info;
	int i;

	for ( i = 0; i < count; i++ ) {
		layouts[i] = layout;
	}
	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	info.descriptorPool = vkt.pool;
	info.descriptorSetCount = count;
	info.pSetLayouts = layouts;
	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &info, sets ) );
}

// writes the image descriptors of bindings 0 to count - 1 of a set; binding
// linear (-1 for none) with the bilinear sampler
static void VK_TemporalWriteSet( VkDescriptorSet set, const VkDescriptorType *types, const VkImageView *views,
	const VkImageLayout *layouts, int count, int linear )
{
	VkDescriptorImageInfo infos[VK_TEMPORAL_MAX_BINDINGS];
	VkWriteDescriptorSet writes[VK_TEMPORAL_MAX_BINDINGS];
	int i;

	Com_Memset( writes, 0, sizeof( writes ) );
	for ( i = 0; i < count; i++ ) {
		infos[i].sampler = i == linear ? vkt.linearSampler : vkt.sampler;
		infos[i].imageView = views[i];
		infos[i].imageLayout = layouts[i];
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = types[i];
		writes[i].pImageInfo = &infos[i];
	}
	qvkUpdateDescriptorSets( vk.device, count, writes, 0, NULL );
}

static const VkDescriptorType vk_prepareTypes[VK_TEMPORAL_PREPARE_BINDINGS] = {
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
};
static const VkDescriptorType vk_hizTypes[VK_TEMPORAL_TAIL_IMAGES] = {
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
};
static const VkDescriptorType vk_compositeTypes[VK_TEMPORAL_COMPOSITE_BINDINGS] = {
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
};
static const VkDescriptorType vk_aoTypes[VK_TEMPORAL_AO_BINDINGS] = {
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
};
static const VkDescriptorType vk_aoApplyTypes[1] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
static const VkDescriptorType vk_mbTypes[VK_TEMPORAL_MB_BINDINGS] = {
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
};
static const VkDescriptorType vk_dofTypes[VK_TEMPORAL_DOF_BINDINGS] = {
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
};
static const VkDescriptorType vk_taaTypes[VK_TEMPORAL_TAA_BINDINGS] = {
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
};

/*
===============
VK_TemporalWriteStaticSets

The sets that don't change until the targets are created again or a group
of images is made or freed (VK_TemporalUpdateFeatures, the GPU idle): those
of the pyramid's dispatches and of the composite, one of each per current
image (three: the scene, the anti-aliased scene, the depth of field's
result), and the ambient occlusion's apply pass's. Frames in flight may bind
them: they are written only then
===============
*/
static void VK_TemporalWriteStaticSets( void ) {
	const VkImageView placeholder = vkt.placeholder.view;
	const qboolean pyramid = ( vkt.groups & VK_TEMPORAL_GROUP_PYRAMID ) != 0;
	const qboolean ao = ( vkt.groups & VK_TEMPORAL_GROUP_AO ) != 0;
	const qboolean prevColor = ( vkt.groups & VK_TEMPORAL_GROUP_PREVCOLOR ) != 0;
	const qboolean mb = ( vkt.groups & VK_TEMPORAL_GROUP_MOTIONBLUR ) != 0;
	const qboolean dof = ( vkt.groups & VK_TEMPORAL_GROUP_DOF ) != 0;
	VkImageView views[VK_TEMPORAL_MAX_BINDINGS];
	VkImageLayout layouts[VK_TEMPORAL_MAX_BINDINGS];
	int cur, i, l;

	for ( cur = 0; cur < VK_TEMPORAL_HISTORY; cur++ ) {
		// the levels a dispatch reads and writes, the last repeated in the
		// bindings it doesn't use
		for ( i = 0; pyramid && i < vkt.numHizPasses; i++ ) {
			const vkHizPass_t *pass = &vkt.hizPasses[i];

			for ( l = 0; l < VK_TEMPORAL_TAIL_IMAGES; l++ ) {
				views[l] = vkt.hizLevel[cur][pass->src + Q_min( l, pass->count )];
				layouts[l] = VK_IMAGE_LAYOUT_GENERAL;
			}
			VK_TemporalWriteSet( vkt.hizSets[cur][i], vk_hizTypes, views, layouts, VK_TEMPORAL_TAIL_IMAGES, -1 );
		}

		// the composite reads all of them in one view or another: the
		// placeholder (GENERAL) for those of the groups not made
		views[0] = vkt.color[prevColor ? cur : 0].view;
		views[1] = vkt.velocity.view;
		views[2] = vkt.normal.view;
		views[3] = pyramid ? vkt.hiz[cur].view : placeholder;
		views[4] = prevColor ? vkt.color[cur ^ 1].view : placeholder;
		views[5] = pyramid ? vkt.hiz[cur ^ 1].view : placeholder;
		views[6] = views[0];
		views[7] = ao ? vkt.aoResult.view : placeholder;
		views[8] = ao ? vkt.aoRaw.view : placeholder;
		views[9] = mb ? vkt.mbVelocity.view : placeholder;
		views[10] = mb ? vkt.mbNeighbor.view : placeholder;
		for ( l = 0; l < VK_TEMPORAL_COMPOSITE_BINDINGS; l++ ) {
			layouts[l] = views[l] == placeholder || l == 3 || l == 5 || l >= 7 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
		VK_TemporalWriteSet( vkt.compositeSets[cur], vk_compositeTypes, views, layouts, VK_TEMPORAL_COMPOSITE_BINDINGS, -1 );

		// the same, showing the anti-aliased scene
		views[0] = ( vkt.groups & VK_TEMPORAL_GROUP_TAA ) ? vkt.taa[cur].view : placeholder;
		layouts[0] = VK_IMAGE_LAYOUT_GENERAL;
		VK_TemporalWriteSet( vkt.compositeTaaSets[cur], vk_compositeTypes, views, layouts, VK_TEMPORAL_COMPOSITE_BINDINGS, -1 );

		// and the depth of field's result
		views[0] = dof ? vkt.dofResult.view : placeholder;
		VK_TemporalWriteSet( vkt.compositeDofSets[cur], vk_compositeTypes, views, layouts, VK_TEMPORAL_COMPOSITE_BINDINGS, -1 );
	}

	// the ambient occlusion's apply pass
	views[0] = ao ? vkt.aoResult.view : placeholder;
	layouts[0] = VK_IMAGE_LAYOUT_GENERAL;
	VK_TemporalWriteSet( vkt.aoApplySet, vk_aoApplyTypes, views, layouts, 1, -1 );
}

/*
===============
VK_TemporalInitPipelines

After VK_InitPipelines and VK_TemporalCreateTargets: the scene's pipeline
layout and shaders (vk_pipeline.c creates its pipelines from
VK_PIPE_TEMPORAL keys), the passes after it, their descriptor sets, the ring
of the skinned draws' frame data
===============
*/
void VK_TemporalInitPipelines( void ) {
	VkDescriptorSetLayoutBinding bindings[4];
	VkDescriptorSetLayoutCreateInfo setLayoutInfo;
	VkDescriptorSetLayout sets[3];
	VkPushConstantRange ranges[2];
	VkDescriptorPoolSize poolSizes[3];
	VkDescriptorPoolCreateInfo poolInfo;
	int i;

	if ( !vkt.enabled ) {
		return;
	}

	// set 2 of the scene's pipelines: the dlights (fragment), the skin data,
	// this frame's and the last frame's frame data (vertex)
	Com_Memset( bindings, 0, sizeof( bindings ) );
	for ( i = 0; i < 4; i++ ) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = i ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	Com_Memset( &setLayoutInfo, 0, sizeof( setLayoutInfo ) );
	setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutInfo.bindingCount = 4;
	setLayoutInfo.pBindings = bindings;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &setLayoutInfo, NULL, &vkt.graphicsSetLayout ) );

	// vkPushConstants_t, then vkTemporalPush_t for the vertex stage: its
	// range covers the fragment stage's, whose pushes then name both stages
	sets[0] = sets[1] = vk.textureSetLayout;
	sets[2] = vkt.graphicsSetLayout;
	ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	ranges[0].offset = 0;
	ranges[0].size = VK_TEMPORAL_PUSH_END;
	ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	ranges[1].offset = VK_PUSH_VERTEX_SIZE;
	ranges[1].size = VK_PUSH_FRAGMENT_SIZE;
	vkt.pipelineLayout = VK_TemporalPipelineLayout( sets, 3, ranges, 2 );

	vkt.vertexShader = VK_CreateShaderModule( vk_spirv_generic_temporal_vert, vk_spirv_generic_temporal_vert_size );
	vkt.skinVertexShader = VK_CreateShaderModule( vk_spirv_skinned_temporal_vert, vk_spirv_skinned_temporal_vert_size );
	vkt.effectsVertexShader = VK_CreateShaderModule( vk_spirv_effects_temporal_vert, vk_spirv_effects_temporal_vert_size );
	if ( vk.enabledFeatures.shaderClipDistance ) {
		vkt.vertexShaderClip = VK_CreateShaderModule( vk_spirv_generic_temporal_clip_vert, vk_spirv_generic_temporal_clip_vert_size );
		vkt.skinVertexShaderClip = VK_CreateShaderModule( vk_spirv_skinned_temporal_clip_vert, vk_spirv_skinned_temporal_clip_vert_size );
		vkt.effectsVertexShaderClip = VK_CreateShaderModule( vk_spirv_effects_temporal_clip_vert, vk_spirv_effects_temporal_clip_vert_size );
	}
	vkt.fragmentShader = VK_CreateShaderModule( vk_spirv_generic_temporal_frag, vk_spirv_generic_temporal_frag_size );

	// the passes after the scene
	vkt.prepareSetLayout = VK_TemporalSetLayout( vk_prepareTypes, VK_TEMPORAL_PREPARE_BINDINGS, VK_SHADER_STAGE_COMPUTE_BIT );
	vkt.hizSetLayout = VK_TemporalSetLayout( vk_hizTypes, VK_TEMPORAL_TAIL_IMAGES, VK_SHADER_STAGE_COMPUTE_BIT );
	vkt.compositeSetLayout = VK_TemporalSetLayout( vk_compositeTypes, VK_TEMPORAL_COMPOSITE_BINDINGS, VK_SHADER_STAGE_FRAGMENT_BIT );
	vkt.prepareLayout = VK_TemporalSimpleLayout( vkt.prepareSetLayout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof( vkPreparePush_t ) );
	vkt.hizLayout = VK_TemporalSimpleLayout( vkt.hizSetLayout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof( vkHizPush_t ) );
	vkt.compositeLayout = VK_TemporalSimpleLayout( vkt.compositeSetLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof( vkCompositePush_t ) );
	vkt.taaSetLayout = VK_TemporalSetLayout( vk_taaTypes, VK_TEMPORAL_TAA_BINDINGS, VK_SHADER_STAGE_COMPUTE_BIT );
	vkt.taaLayout = VK_TemporalSimpleLayout( vkt.taaSetLayout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof( vkTaaPush_t ) );

	vkt.prepareShader = VK_CreateShaderModule( vk_spirv_hiz_prepare_comp, vk_spirv_hiz_prepare_comp_size );
	vkt.hizShader = VK_CreateShaderModule( vk_spirv_hiz_build_comp, vk_spirv_hiz_build_comp_size );
	vkt.hizTailShader = VK_CreateShaderModule( vk_spirv_hiz_tail_comp, vk_spirv_hiz_tail_comp_size );
	vkt.compositeVertexShader = VK_CreateShaderModule( vk_spirv_temporal_composite_vert, vk_spirv_temporal_composite_vert_size );
	vkt.compositeFragmentShader = VK_CreateShaderModule( vk_spirv_temporal_composite_frag, vk_spirv_temporal_composite_frag_size );
	vkt.preparePipeline = VK_TemporalComputePipeline( vkt.prepareShader, vkt.prepareLayout );
	vkt.hizPipeline = VK_TemporalComputePipeline( vkt.hizShader, vkt.hizLayout );
	vkt.hizTailPipeline = VK_TemporalComputePipeline( vkt.hizTailShader, vkt.hizLayout );
	vkt.compositePipeline = VK_TemporalCompositePipeline( VK_FALSE );
	vkt.compositeBlurPipeline = VK_TemporalCompositePipeline( VK_TRUE );
	vkt.taaShader = VK_CreateShaderModule( vk_spirv_taa_resolve_comp, vk_spirv_taa_resolve_comp_size );
	vkt.taaPipeline = VK_TemporalComputePipeline( vkt.taaShader, vkt.taaLayout );

	// the ambient occlusion: one set and layout for its compute passes
	vkt.aoSetLayout = VK_TemporalSetLayout( vk_aoTypes, VK_TEMPORAL_AO_BINDINGS, VK_SHADER_STAGE_COMPUTE_BIT );
	vkt.aoLayout = VK_TemporalSimpleLayout( vkt.aoSetLayout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof( vkAoPush_t ) );
	vkt.aoApplySetLayout = VK_TemporalSetLayout( vk_aoApplyTypes, 1, VK_SHADER_STAGE_FRAGMENT_BIT );
	vkt.aoApplyLayout = VK_TemporalPipelineLayout( &vkt.aoApplySetLayout, 1, NULL, 0 );
	vkt.aoPrefilterShader = VK_CreateShaderModule( vk_spirv_ao_prefilter_comp, vk_spirv_ao_prefilter_comp_size );
	vkt.aoMainShader = VK_CreateShaderModule( vk_spirv_gtao_main_comp, vk_spirv_gtao_main_comp_size );
	vkt.aoTemporalShader = VK_CreateShaderModule( vk_spirv_ao_temporal_comp, vk_spirv_ao_temporal_comp_size );
	vkt.aoDenoiseShader = VK_CreateShaderModule( vk_spirv_ao_denoise_comp, vk_spirv_ao_denoise_comp_size );
	vkt.aoApplyShader = VK_CreateShaderModule( vk_spirv_ao_apply_frag, vk_spirv_ao_apply_frag_size );
	vkt.aoPrefilterPipeline = VK_TemporalComputePipeline( vkt.aoPrefilterShader, vkt.aoLayout );
	for ( i = 0; i < 4; i++ ) {
		vkt.aoMainPipelines[i] = VK_TemporalAoMainPipeline( i );
	}
	vkt.aoTemporalPipeline = VK_TemporalComputePipeline( vkt.aoTemporalShader, vkt.aoLayout );
	vkt.aoDenoisePipeline = VK_TemporalComputePipeline( vkt.aoDenoiseShader, vkt.aoLayout );
	vkt.aoApplyPipeline = VK_TemporalAoApplyPipeline();

	// the motion blur: one set and layout for its two passes
	vkt.mbSetLayout = VK_TemporalSetLayout( vk_mbTypes, VK_TEMPORAL_MB_BINDINGS, VK_SHADER_STAGE_COMPUTE_BIT );
	vkt.mbLayout = VK_TemporalSimpleLayout( vkt.mbSetLayout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof( vkMotionBlurPush_t ) );
	vkt.mbTilesShader = VK_CreateShaderModule( vk_spirv_motion_blur_tiles_comp, vk_spirv_motion_blur_tiles_comp_size );
	vkt.mbNeighborShader = VK_CreateShaderModule( vk_spirv_motion_blur_neighbor_comp, vk_spirv_motion_blur_neighbor_comp_size );
	vkt.mbTilesPipeline = VK_TemporalComputePipeline( vkt.mbTilesShader, vkt.mbLayout );
	vkt.mbNeighborPipeline = VK_TemporalComputePipeline( vkt.mbNeighborShader, vkt.mbLayout );

	// the depth of field: one set and layout for its five passes
	vkt.dofSetLayout = VK_TemporalSetLayout( vk_dofTypes, VK_TEMPORAL_DOF_BINDINGS, VK_SHADER_STAGE_COMPUTE_BIT );
	vkt.dofLayout = VK_TemporalSimpleLayout( vkt.dofSetLayout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof( vkDofPush_t ) );
	vkt.dofFocusShader = VK_CreateShaderModule( vk_spirv_dof_focus_comp, vk_spirv_dof_focus_comp_size );
	vkt.dofSetupShader = VK_CreateShaderModule( vk_spirv_dof_setup_comp, vk_spirv_dof_setup_comp_size );
	vkt.dofNeighborShader = VK_CreateShaderModule( vk_spirv_dof_neighbor_comp, vk_spirv_dof_neighbor_comp_size );
	vkt.dofGatherShader = VK_CreateShaderModule( vk_spirv_dof_gather_comp, vk_spirv_dof_gather_comp_size );
	vkt.dofApplyShader = VK_CreateShaderModule( vk_spirv_dof_apply_comp, vk_spirv_dof_apply_comp_size );
	vkt.dofFocusPipeline = VK_TemporalComputePipeline( vkt.dofFocusShader, vkt.dofLayout );
	vkt.dofSetupPipeline = VK_TemporalComputePipeline( vkt.dofSetupShader, vkt.dofLayout );
	vkt.dofNeighborPipeline = VK_TemporalComputePipeline( vkt.dofNeighborShader, vkt.dofLayout );
	vkt.dofGatherPipeline = VK_TemporalComputePipeline( vkt.dofGatherShader, vkt.dofLayout );
	vkt.dofApplyPipeline = VK_TemporalComputePipeline( vkt.dofApplyShader, vkt.dofLayout );

	// per frame slot: set 2 of the scene, the prepare pass's, the
	// anti-aliasing's, the ambient occlusion's, the motion blur's and the depth of field's sets;
	// per current image: the pyramid's dispatches' and the composite's three; the ambient
	// occlusion's apply pass's
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSizes[0].descriptorCount = VK_MAX_FRAMES * 4;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[1].descriptorCount = VK_MAX_FRAMES * ( 2 + VK_TEMPORAL_TAA_BINDINGS - 2 + VK_TEMPORAL_AO_SAMPLED + VK_TEMPORAL_MB_SAMPLED + VK_TEMPORAL_DOF_SAMPLED )
		+ VK_TEMPORAL_HISTORY * 3 * VK_TEMPORAL_COMPOSITE_BINDINGS + 1;
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[2].descriptorCount = VK_MAX_FRAMES * ( 4 + VK_TEMPORAL_AO_BINDINGS - VK_TEMPORAL_AO_SAMPLED + VK_TEMPORAL_MB_BINDINGS - VK_TEMPORAL_MB_SAMPLED
		+ VK_TEMPORAL_DOF_BINDINGS - VK_TEMPORAL_DOF_SAMPLED )
		+ VK_TEMPORAL_HISTORY * VK_TEMPORAL_MAX_PASSES * VK_TEMPORAL_TAIL_IMAGES;
	Com_Memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = VK_MAX_FRAMES * 6 + VK_TEMPORAL_HISTORY * ( VK_TEMPORAL_MAX_PASSES + 3 ) + 1;
	poolInfo.poolSizeCount = ARRAY_LEN( poolSizes );
	poolInfo.pPoolSizes = poolSizes;
	VK_CHECK( qvkCreateDescriptorPool( vk.device, &poolInfo, NULL, &vkt.pool ) );

	VK_TemporalAllocateSets( vkt.graphicsSetLayout, vkt.graphicsSets, VK_MAX_FRAMES );
	VK_TemporalAllocateSets( vkt.prepareSetLayout, vkt.prepareSets, VK_MAX_FRAMES );
	VK_TemporalAllocateSets( vkt.compositeSetLayout, vkt.compositeSets, VK_TEMPORAL_HISTORY );
	VK_TemporalAllocateSets( vkt.compositeSetLayout, vkt.compositeTaaSets, VK_TEMPORAL_HISTORY );
	VK_TemporalAllocateSets( vkt.compositeSetLayout, vkt.compositeDofSets, VK_TEMPORAL_HISTORY );
	VK_TemporalAllocateSets( vkt.taaSetLayout, vkt.taaSets, VK_MAX_FRAMES );
	VK_TemporalAllocateSets( vkt.aoSetLayout, vkt.aoSets, VK_MAX_FRAMES );
	VK_TemporalAllocateSets( vkt.aoApplySetLayout, &vkt.aoApplySet, 1 );
	VK_TemporalAllocateSets( vkt.mbSetLayout, vkt.mbSets, VK_MAX_FRAMES );
	VK_TemporalAllocateSets( vkt.dofSetLayout, vkt.dofSets, VK_MAX_FRAMES );
	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		if ( vkt.numHizPasses ) {
			VK_TemporalAllocateSets( vkt.hizSetLayout, vkt.hizSets[i], vkt.numHizPasses );
		}
	}
	VK_TemporalWriteStaticSets();

	// the frame slots' buffers and one more
	for ( i = 0; i < VK_MAX_FRAMES; i++ ) {
		vkt.skinRing[i] = &vk.frames[i].skinBuffer;
	}
	VK_CreateBuffer( &vkt.skinExtra, VK_SKIN_FRAME_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	vkt.skinRing[VK_MAX_FRAMES] = &vkt.skinExtra;
	vkt.skinSlot = 0;
}

void VK_TemporalShutdownPipelines( void ) {
	VkShaderModule *modules[] = {
		&vkt.vertexShader, &vkt.vertexShaderClip, &vkt.skinVertexShader, &vkt.skinVertexShaderClip,
		&vkt.effectsVertexShader, &vkt.effectsVertexShaderClip, &vkt.fragmentShader,
		&vkt.prepareShader, &vkt.hizShader, &vkt.hizTailShader, &vkt.compositeVertexShader, &vkt.compositeFragmentShader,
		&vkt.taaShader, &vkt.aoPrefilterShader, &vkt.aoMainShader, &vkt.aoTemporalShader, &vkt.aoDenoiseShader, &vkt.aoApplyShader,
		&vkt.mbTilesShader, &vkt.mbNeighborShader,
		&vkt.dofFocusShader, &vkt.dofSetupShader, &vkt.dofNeighborShader, &vkt.dofGatherShader, &vkt.dofApplyShader
	};
	VkPipeline *pipelines[] = { &vkt.preparePipeline, &vkt.hizPipeline, &vkt.hizTailPipeline, &vkt.compositePipeline, &vkt.taaPipeline,
		&vkt.aoPrefilterPipeline, &vkt.aoMainPipelines[0], &vkt.aoMainPipelines[1], &vkt.aoMainPipelines[2], &vkt.aoMainPipelines[3],
		&vkt.aoTemporalPipeline, &vkt.aoDenoisePipeline, &vkt.aoApplyPipeline, &vkt.compositeBlurPipeline, &vkt.mbTilesPipeline,
		&vkt.mbNeighborPipeline, &vkt.dofFocusPipeline, &vkt.dofSetupPipeline, &vkt.dofNeighborPipeline, &vkt.dofGatherPipeline,
		&vkt.dofApplyPipeline };
	VkPipelineLayout *pipelineLayouts[] = { &vkt.pipelineLayout, &vkt.prepareLayout, &vkt.hizLayout, &vkt.compositeLayout, &vkt.taaLayout,
		&vkt.aoLayout, &vkt.aoApplyLayout, &vkt.mbLayout, &vkt.dofLayout };
	VkDescriptorSetLayout *setLayouts[] = { &vkt.graphicsSetLayout, &vkt.prepareSetLayout, &vkt.hizSetLayout, &vkt.compositeSetLayout,
		&vkt.taaSetLayout, &vkt.aoSetLayout, &vkt.aoApplySetLayout, &vkt.mbSetLayout, &vkt.dofSetLayout };
	unsigned int i;

	if ( !vk.device ) {
		return;
	}
	for ( i = 0; i < ARRAY_LEN( pipelines ); i++ ) {
		if ( *pipelines[i] ) {
			qvkDestroyPipeline( vk.device, *pipelines[i], NULL );
			*pipelines[i] = VK_NULL_HANDLE;
		}
	}
	for ( i = 0; i < ARRAY_LEN( modules ); i++ ) {
		if ( *modules[i] ) {
			qvkDestroyShaderModule( vk.device, *modules[i], NULL );
			*modules[i] = VK_NULL_HANDLE;
		}
	}
	for ( i = 0; i < ARRAY_LEN( pipelineLayouts ); i++ ) {
		if ( *pipelineLayouts[i] ) {
			qvkDestroyPipelineLayout( vk.device, *pipelineLayouts[i], NULL );
			*pipelineLayouts[i] = VK_NULL_HANDLE;
		}
	}
	if ( vkt.pool ) {
		qvkDestroyDescriptorPool( vk.device, vkt.pool, NULL );
		vkt.pool = VK_NULL_HANDLE;
	}
	for ( i = 0; i < ARRAY_LEN( setLayouts ); i++ ) {
		if ( *setLayouts[i] ) {
			qvkDestroyDescriptorSetLayout( vk.device, *setLayouts[i], NULL );
			*setLayouts[i] = VK_NULL_HANDLE;
		}
	}
	// the frame slots' buffers go with the frames
	VK_DestroyBuffer( &vkt.skinExtra );
	Com_Memset( vkt.skinRing, 0, sizeof( vkt.skinRing ) );
	Com_Memset( vkt.graphicsSets, 0, sizeof( vkt.graphicsSets ) );
	Com_Memset( vkt.prepareSets, 0, sizeof( vkt.prepareSets ) );
	Com_Memset( vkt.hizSets, 0, sizeof( vkt.hizSets ) );
	Com_Memset( vkt.compositeSets, 0, sizeof( vkt.compositeSets ) );
	Com_Memset( vkt.compositeTaaSets, 0, sizeof( vkt.compositeTaaSets ) );
	Com_Memset( vkt.compositeDofSets, 0, sizeof( vkt.compositeDofSets ) );
	Com_Memset( vkt.taaSets, 0, sizeof( vkt.taaSets ) );
	Com_Memset( vkt.aoSets, 0, sizeof( vkt.aoSets ) );
	Com_Memset( vkt.mbSets, 0, sizeof( vkt.mbSets ) );
	Com_Memset( vkt.dofSets, 0, sizeof( vkt.dofSets ) );
	vkt.aoApplySet = VK_NULL_HANDLE;
}

/*
====================================================================

FRAMES AND SCENES

====================================================================
*/

static void VK_TemporalWriteBuffer( VkWriteDescriptorSet *write, VkDescriptorBufferInfo *info, VkDescriptorSet set, int binding, VkBuffer buffer ) {
	info->buffer = buffer;
	info->offset = 0;
	info->range = VK_WHOLE_SIZE;
	Com_Memset( write, 0, sizeof( *write ) );
	write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write->dstSet = set;
	write->dstBinding = binding;
	write->descriptorCount = 1;
	write->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write->pBufferInfo = info;
}

/*
===============
VK_TemporalBeginFrame

At the start of a frame, once its slot is free: the frame data of the
skinned draws goes to the next buffer of the ring, and set 2 of the slot
(both the one of the normal pipelines and the scene's) points at it; the
scene's also at the last frame's
===============
*/
void VK_TemporalBeginFrame( vkFrame_t *frame, VkBuffer skinData ) {
	VkWriteDescriptorSet writes[5];
	VkDescriptorBufferInfo infos[5];
	VkDescriptorSet set;
	int prev;

	if ( !vkt.enabled || !vkt.graphicsSets[0] || !frame->dlightSet ) {
		return;
	}
	vkt.skinSlot = (int)( frame->serial % VK_SKIN_RING );
	prev = (int)( ( frame->serial + VK_SKIN_RING - 1 ) % VK_SKIN_RING );
	set = vkt.graphicsSets[frame - vk.frames];

	VK_TemporalWriteBuffer( &writes[0], &infos[0], frame->dlightSet, 2, vkt.skinRing[vkt.skinSlot]->buffer );
	VK_TemporalWriteBuffer( &writes[1], &infos[1], set, 0, frame->dlightBuffer.buffer );
	VK_TemporalWriteBuffer( &writes[2], &infos[2], set, 1, skinData );
	VK_TemporalWriteBuffer( &writes[3], &infos[3], set, 2, vkt.skinRing[vkt.skinSlot]->buffer );
	VK_TemporalWriteBuffer( &writes[4], &infos[4], set, 3, vkt.skinRing[prev]->buffer );
	qvkUpdateDescriptorSets( vk.device, 5, writes, 0, NULL );
}

static void VK_TemporalImageBarrier( VkImageMemoryBarrier *b, VkImage image, VkImageAspectFlags aspect,
	VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess )
{
	Com_Memset( b, 0, sizeof( *b ) );
	b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b->srcAccessMask = srcAccess;
	b->dstAccessMask = dstAccess;
	b->oldLayout = oldLayout;
	b->newLayout = newLayout;
	b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b->image = image;
	b->subresourceRange.aspectMask = aspect;
	b->subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	b->subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
}

// what compute shaders wrote, to the stages that read (or write) it next
static void VK_TemporalComputeBarrier( VkCommandBuffer cmd, VkPipelineStageFlags dstStages ) {
	VkMemoryBarrier memory;

	Com_Memset( &memory, 0, sizeof( memory ) );
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dstStages, 0, 1, &memory, 0, NULL, 0, NULL );
}

/*
===============
VK_TemporalConsumers

Where what resolves the scene goes, between the pyramid and the composite,
outside of a render pass: this frame's color, motion vectors and normals,
the last frame's color (when vkt.historySerial is the frame before and
vkt.view.params[2] says the view has a last frame), the view (vkt.view,
TemporalView of shaders/temporal_common.glsl, with its camera motion and
derived normals), the jitter (vkt.jitterPixels); with VK_TEMPORAL_READ_DEPTH
in vkt.readers (set it when the reader is created) the depth image
(vkt.depthView, read only) and this frame's depth pyramid, and the last
frame's when vkt.hizSerial says it was made in the frame before.

The ambient occlusion (r_ao) is not here: it is in the scene before what is
blended over its opaque surfaces (VK_TemporalAo, vk_ao_scene).

Temporal anti-aliasing (VK_TemporalTaa, which needs the depth): this frame's
scene and the last frame's result (taa[current ^ 1], when it is the frame
before's and the view has a last frame) into taa[current], which the
composite shows then (vkt.taaRan); with level0 it also makes level 0 of this
frame's pyramid, which no pass made then (its only reader is the next
frame's anti-aliasing). VK_TemporalResolve puts taa[current] into GENERAL
before and makes its writes visible after.
===============
*/
static void VK_TemporalConsumers( VkCommandBuffer cmd, qboolean level0 ) {
	const int cur = vkt.current;
	const VkDescriptorSet set = vkt.taaSets[vk.frameIndex];
	VkImageView views[VK_TEMPORAL_TAA_BINDINGS];
	VkImageLayout layouts[VK_TEMPORAL_TAA_BINDINGS];
	vkTaaPush_t push;
	float sum;
	int x, y;

	// the depth image's view can change between frames; this frame slot's
	// last use of the set is done
	views[0] = vkt.color[vkt.colorIndex].view;
	views[1] = vkt.velocity.view;
	views[2] = vkt.normal.view;
	views[3] = vkt.depthView;
	views[4] = vkt.taa[cur ^ 1].view;
	views[5] = vkt.hiz[cur ^ 1].view;
	views[6] = vkt.taa[cur].view;
	views[7] = vkt.hizLevel[cur][0];
	layouts[0] = layouts[1] = layouts[2] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	layouts[3] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	layouts[4] = layouts[5] = layouts[6] = layouts[7] = VK_IMAGE_LAYOUT_GENERAL;
	VK_TemporalWriteSet( set, vk_taaTypes, views, layouts, VK_TEMPORAL_TAA_BINDINGS, VK_TEMPORAL_TAA_HISTORY_BINDING );

	Com_Memset( &push, 0, sizeof( push ) );
	push.view = vkt.view;
	Com_Memcpy( push.rect, vkt.rect, sizeof( push.rect ) );
	push.params[0] = r_taaFeedback->value;
	push.params[1] = vkt.taaSerial[cur ^ 1] && vkt.taaSerial[cur ^ 1] + 1 == vk.frameSerial && vkt.view.params[2] != 0.0f ? 1.0f : 0.0f;
	// this frame's color at the pixel's center: each pixel's sample is at its
	// center minus the jitter, weighted by its distance to the center (a
	// Gaussian close to Blackman-Harris), the same for every pixel
	for ( y = 0, sum = 0.0f; y < 3; y++ ) {
		for ( x = 0; x < 3; x++ ) {
			float ox = x - 1 - vkt.jitterPixels[0];
			float oy = y - 1 - vkt.jitterPixels[1];

			push.weights[y * 3 + x] = expf( -2.29f * ( ox * ox + oy * oy ) );
			sum += push.weights[y * 3 + x];
		}
	}
	for ( x = 0; x < 9; x++ ) {
		push.weights[x] /= sum;
	}
	push.params[2] = vkt.hizSerial[cur ^ 1] && vkt.hizSerial[cur ^ 1] + 1 == vk.frameSerial ? 1.0f : 0.0f;
	push.params[3] = level0 ? 1.0f : 0.0f;

	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.taaPipeline );
	qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.taaLayout, 0, 1, &set, 0, NULL );
	qvkCmdPushConstants( cmd, vkt.taaLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );
	qvkCmdDispatch( cmd, ( vkt.rect[2] + VK_TEMPORAL_TAA_GROUP - 1 ) / VK_TEMPORAL_TAA_GROUP,
		( vkt.rect[3] + VK_TEMPORAL_TAA_GROUP - 1 ) / VK_TEMPORAL_TAA_GROUP, 1 );

	vkt.taaSerial[cur] = vk.frameSerial;
	vkt.taaRan = qtrue;
}

// the far distance of the primary view's projection (GL: z_ndc 1)
static float VK_TemporalFar( void ) {
	const float *p = vkt.projection;

	return fabs( 1.0f + p[10] ) > 1e-9f ? p[14] / ( 1.0f + p[10] ) : 65536.0f;
}

/*
===============
VK_TemporalReaders

What the readers of this scene's buffers need: theirs (vkt.readers), and
what r_temporalShow's view reads (the motion vectors and the normals of what
the draws left to the readers, the pyramid itself). The anti-aliasing's
(VK_TemporalTaa) are apart: the depth image and level 0 of the pyramid only,
which it makes itself when nothing else needs the pyramid
===============
*/
static int VK_TemporalReadersFor( int showMode ) {
	int readers = vkt.readers;

	switch ( showMode ) {
	case 1: case 2: case 4: case 6: case 7:
		readers |= VK_TEMPORAL_READ_DEPTH;
		break;
	}
	return readers;
}

// for this scene (r_temporalShow latched)
static int VK_TemporalReaders( void ) {
	return VK_TemporalReadersFor( vkt.showMode );
}

/*
===============
VK_TemporalUpdateFeatures

At the start of a frame's scene (vk_begin_scene, outside of a render pass,
before anything of the scene is recorded): which readers of the scene are on
(r_taa, r_ao, r_temporalShow's views), latched for the scene, and the groups
of images only they read made or freed to match (VK_TEMPORAL_GROUP_*): about
270 MB at 2560x1440 with all of them, 44 without (the color, the motion
vectors, the normals). A change waits for the GPU (nothing in flight may use
what goes, or the static sets rewritten): a few milliseconds when a cvar
changes, never otherwise
===============
*/
void VK_TemporalUpdateFeatures( void ) {
	static const char *names[] = { "anti-aliasing (r_taa)", "depth pyramid", "ambient occlusion (r_ao)", "last scene's color (r_temporalShow 5)",
		"motion blur (r_motionBlur)", "depth of field (r_dof)" };
	const qboolean mbShow = r_temporalShow->integer == 11 || r_temporalShow->integer == 12;
	int mode = r_temporalShow->integer;
	int need = 0, change, group, i;

	if ( r_taa->integer ) {
		need |= VK_TEMPORAL_GROUP_TAA | VK_TEMPORAL_GROUP_PYRAMID;	// the last frame's depth
	}
	if ( r_ao->integer ) {
		need |= VK_TEMPORAL_GROUP_AO;
	}
	if ( VK_TemporalReadersFor( mode ) & VK_TEMPORAL_READ_DEPTH ) {
		need |= VK_TEMPORAL_GROUP_PYRAMID;
	}
	if ( mode == 5 ) {
		need |= VK_TEMPORAL_GROUP_PREVCOLOR;
	}
	if ( r_motionBlur->integer || mbShow ) {
		need |= VK_TEMPORAL_GROUP_MOTIONBLUR;
	}
	if ( r_dof->integer || mode == 13 ) {
		need |= VK_TEMPORAL_GROUP_DOF;
	}

	change = need ^ vkt.groups;
	if ( change ) {
		vk_finish();
		for ( group = 1, i = 0; group <= VK_TEMPORAL_GROUP_LAST; group <<= 1, i++ ) {
			if ( !( change & group ) ) {
				continue;
			}
			if ( need & group ) {
				VK_TemporalCreateGroup( group, vk.frame->mainCmd );
			} else {
				VK_TemporalDestroyGroup( group );
			}
			ri.Printf( PRINT_DEVELOPER, "r_temporal: the images of the %s %s\n", names[i], ( need & group ) ? "made" : "freed" );
		}
		if ( change & VK_TEMPORAL_GROUP_PREVCOLOR ) {
			// the framebuffers: one color image, or two that alternate
			for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
				qvkDestroyFramebuffer( vk.device, vkt.framebuffers[i], NULL );
				vkt.framebuffers[i] = VK_NULL_HANDLE;
			}
			qvkDestroyImageView( vk.device, vkt.depthView, NULL );
			vkt.depthView = VK_NULL_HANDLE;
			VK_TemporalCreateDepthObjects();
		}
		VK_TemporalWriteStaticSets();
	}

	vkt.showMode = mode;
	vkt.aoOn = r_ao->integer && ( vkt.groups & VK_TEMPORAL_GROUP_AO );
	vkt.mbOn = ( r_motionBlur->integer || mbShow ) && ( vkt.groups & VK_TEMPORAL_GROUP_MOTIONBLUR );
	vkt.mbBlur = vkt.mbOn && r_motionBlur->integer;
	vkt.dofOn = ( r_dof->integer || mode == 13 ) && ( vkt.groups & VK_TEMPORAL_GROUP_DOF );
	vkt.dofShow = vkt.dofOn && mode == 13;
	vkt.colorIndex = ( vkt.groups & VK_TEMPORAL_GROUP_PREVCOLOR ) ? vkt.current : 0;
}

// whether the anti-aliasing resolves this scene
static qboolean VK_TemporalTaa( void ) {
	return vkt.taaOn && vkt.rect[2] && vkt.rect[3];
}

// the view's depth range and far distance, as the scene's draws used them
static void VK_TemporalViewDepth( void ) {
	vkt.view.depth[0] = vkt.projection[10];
	vkt.view.depth[1] = vkt.projection[14];
	vkt.view.depth[2] = vk_depth_floor();
	vkt.view.depth[3] = VK_TemporalFar();
}

/*
====================================================================

AMBIENT OCCLUSION

====================================================================
*/

// whether this scene's ambient occlusion is still to come (r_ao)
qboolean VK_TemporalAoWanted( void ) {
	return vkt.enabled && vkt.active && vkt.aoOn && !vkt.aoRan && vkt.aoSets[0];
}

/*
===============
VK_TemporalAo

Ground truth ambient occlusion of the primary view, outside of a render
pass, after the view's opaque surfaces (vk_ao_scene; the scene's images are
in their attachment layouts): the depth buffer to view distances
(ao_prefilter.comp), the occlusion (gtao_main.comp), blended with the last
frames' (ao_temporal.comp), blurred into what the apply pass blends
(ao_denoise.comp, once or twice). The depth, normal and motion images are
the passes' to read in between, then attachments again. fog: the view's
draws' (start, end, r, g, b), NULL for none. Sets vkt.aoRan when it ran
===============
*/
void VK_TemporalAo( VkCommandBuffer cmd, const float *fog ) {
	const int cur = vkt.current;
	const VkDescriptorSet set = vkt.aoSets[vk.frameIndex];
	const VkPipelineStageFlags depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	const int *rect = vkt.view.rect;
	VkImageView views[VK_TEMPORAL_AO_BINDINGS];
	VkImageLayout layouts[VK_TEMPORAL_AO_BINDINGS];
	VkImageMemoryBarrier barriers[3];
	VkMemoryBarrier memory;
	vkAoPush_t push;
	qboolean history, twoBlurs;
	int q, i, groupsX, groupsY;

	if ( !rect[2] || !rect[3] ) {
		return;
	}
	VK_TemporalViewDepth();

	// the depth image's view can change between frames; this frame slot's
	// last use of the set is done
	views[0] = vkt.depthView;
	views[1] = vkt.normal.view;
	views[2] = vkt.velocity.view;
	views[3] = vkt.aoDepth[cur].view;
	views[4] = vkt.aoDepth[cur ^ 1].view;
	views[5] = vkt.aoRaw.view;
	views[6] = vkt.aoHistory[cur ^ 1].view;
	for ( i = 0; i < VK_TEMPORAL_AO_LEVELS; i++ ) {
		views[7 + i] = vkt.aoDepthLevel[cur][i];
	}
	views[12] = vkt.aoRaw.view;
	views[13] = vkt.aoHistory[cur].view;
	views[14] = vkt.aoBlur.view;
	views[15] = vkt.aoResult.view;
	views[16] = vkt.aoHistory[cur].view;
	views[17] = vkt.aoBlur.view;
	for ( i = 0; i < VK_TEMPORAL_AO_BINDINGS; i++ ) {
		layouts[i] = VK_IMAGE_LAYOUT_GENERAL;
	}
	layouts[0] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	layouts[1] = layouts[2] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VK_TemporalWriteSet( set, vk_aoTypes, views, layouts, VK_TEMPORAL_AO_BINDINGS, -1 );

	Com_Memset( &push, 0, sizeof( push ) );
	push.view = vkt.view;
	q = Q_min( Q_max( r_aoQuality->integer, 0 ), 3 );
	history = vkt.aoSerial[cur ^ 1] && vkt.aoSerial[cur ^ 1] + 1 == vk.frameSerial && vkt.view.params[2] != 0.0f && r_aoTemporal->integer > 1;
	push.ao[0] = r_aoRadius->value;
	push.ao[1] = r_aoFalloff->value;
	push.ao[2] = r_aoThickness->value;
	push.ao[3] = r_aoStrength->value;
	push.ao2[0] = r_aoMultiBounce->value;
	push.ao2[1] = history ? 1.0f : 0.0f;
	push.ao2[2] = (float)r_aoTemporal->integer;
	push.ao2[3] = 1.2f;		// XeGTAO's DenoiseBlurBeta
	if ( fog ) {
		// as the draws' (vk.c: Mesa's optimized fog parameters)
		float scale = fog[1] == fog[0] ? 1.0f : -1.0f / ( fog[1] - fog[0] );

		push.fogParams[0] = scale;
		push.fogParams[1] = fog[1] * -scale;
		push.fogColor[0] = Com_Clamp( 0.0f, 1.0f, fog[2] );
		push.fogColor[1] = Com_Clamp( 0.0f, 1.0f, fog[3] );
		push.fogColor[2] = Com_Clamp( 0.0f, 1.0f, fog[4] );
		push.fogColor[3] = 1.0f;
	}
	push.fogParams[2] = r_aoNormals->integer ? 1.0f : 0.0f;
	push.fogParams[3] = vkt.aoDepthSerial[cur ^ 1] && vkt.aoDepthSerial[cur ^ 1] + 1 == vk.frameSerial ? 1.0f : 0.0f;
	push.ints[0] = vk_aoQuality[q][0];
	push.ints[1] = vk_aoQuality[q][1];
	// the noise moves with the frames only when they are blended
	push.ints[2] = r_aoTemporal->integer > 1 ? (int)( vk.frameSerial & 63 ) : 0;
	twoBlurs = q == 3 || !history;
	push.ints[3] = twoBlurs ? 0 : 1;	// AO_PASS_HISTORY_TO_BLUR, AO_PASS_HISTORY_TO_RESULT

	// what the scene drew so far, to the passes; the ambient occlusion's
	// images were last read by the last frame's passes and composite
	VK_TemporalImageBarrier( &barriers[0], vk.depthImage, vk.depthAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	VK_TemporalImageBarrier( &barriers[1], vkt.normal.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	VK_TemporalImageBarrier( &barriers[2], vkt.velocity.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	Com_Memset( &memory, 0, sizeof( memory ) );
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | depthStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
		| VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, NULL, 3, barriers );

	qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.aoLayout, 0, 1, &set, 0, NULL );
	qvkCmdPushConstants( cmd, vkt.aoLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );

	// the view distances of the whole framebuffer (the levels line up with
	// it), 16 x 16 pixels a workgroup
	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.aoPrefilterPipeline );
	qvkCmdDispatch( cmd, ( vk.width + 15 ) / 16, ( vk.height + 15 ) / 16, 1 );
	VK_TemporalComputeBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

	// the others over the primary view, 8 x 8 pixels a workgroup
	groupsX = ( rect[2] + 7 ) / 8;
	groupsY = ( rect[3] + 7 ) / 8;
	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.aoMainPipelines[q] );
	qvkCmdDispatch( cmd, groupsX, groupsY, 1 );
	VK_TemporalComputeBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

	// blended over frames and blurred, 16 x 16 pixels a workgroup: into the
	// result, or, at r_aoQuality 3 and without history (the first frames
	// after a cut are the noisiest), into o_blur for a second blur
	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.aoTemporalPipeline );
	qvkCmdDispatch( cmd, ( rect[2] + 15 ) / 16, ( rect[3] + 15 ) / 16, 1 );
	if ( twoBlurs ) {
		VK_TemporalComputeBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
		push.ints[3] = 2;	// AO_PASS_BLUR_TO_RESULT
		qvkCmdPushConstants( cmd, vkt.aoLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );
		qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.aoDenoisePipeline );
		qvkCmdDispatch( cmd, groupsX, groupsY, 1 );
	}

	// the scene's images back to the draws; the result to the apply pass
	// (and the composite's r_temporalShow views)
	VK_TemporalImageBarrier( &barriers[0], vk.depthImage, vk.depthAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
	VK_TemporalImageBarrier( &barriers[1], vkt.normal.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
	VK_TemporalImageBarrier( &barriers[2], vkt.velocity.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | depthStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &memory, 0, NULL, 3, barriers );

	vkt.aoDepthSerial[cur] = vk.frameSerial;
	vkt.aoSerial[cur] = vk.frameSerial;
	vkt.aoRan = qtrue;
}

/*
===============
VK_TemporalAoApply

In the scene's render pass, after VK_TemporalAo: its result blended into
the scene's color over the primary view (ao_apply.frag). The caller rebinds
what it draws with next
===============
*/
void VK_TemporalAoApply( VkCommandBuffer cmd ) {
	VkViewport viewport;
	VkRect2D scissor;

	viewport.x = 0;
	viewport.y = 0;
	viewport.width = vk.width;
	viewport.height = vk.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	scissor.offset.x = vkt.view.rect[0];
	scissor.offset.y = vkt.view.rect[1];
	scissor.extent.width = vkt.view.rect[2];
	scissor.extent.height = vkt.view.rect[3];

	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkt.aoApplyPipeline );
	qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkt.aoApplyLayout, 0, 1, &vkt.aoApplySet, 0, NULL );
	qvkCmdSetViewport( cmd, 0, 1, &viewport );
	qvkCmdSetScissor( cmd, 0, 1, &scissor );
	qvkCmdDraw( cmd, 3, 1, 0, 0 );
}

/*
====================================================================

MOTION BLUR

====================================================================
*/

// whether this scene's motion blur passes run (r_motionBlur, r_temporalShow 11, 12)
static qboolean VK_TemporalBlurs( void ) {
	return vkt.mbOn && vkt.mbSets[0] && vkt.rect[2] && vkt.rect[3];
}

/*
===============
VK_TemporalMotionBlur

In VK_TemporalResolve after the anti-aliasing (the depth image, the normals
and the motion vectors readable): each pixel's blur and the longest of each
tile (motion_blur.comp), then the longest of the 3 x 3 tiles around each
(NEIGHBOR), which the composite gathers along (temporal_composite.frag,
MOTION_BLUR). The blur is the motion of an exposure of r_motionBlurShutter
frames at r_motionBlurFps: the frame's motion times that exposure over the
game time between the frames (vkt.frameMsec, none without it); with
r_motionBlurFps 0 the frame's own motion times r_motionBlurShutter. Its
images were last read by the last frame's composite: the barrier before the
resolve's passes waits for it. Sets vkt.mbRan
===============
*/
static void VK_TemporalMotionBlur( VkCommandBuffer cmd ) {
	const VkDescriptorSet set = vkt.mbSets[vk.frameIndex];
	VkImageView views[VK_TEMPORAL_MB_BINDINGS];
	VkImageLayout layouts[VK_TEMPORAL_MB_BINDINGS];
	vkMotionBlurPush_t push;
	float exposure;
	int i;

	// the depth image's view can change between frames; this frame slot's
	// last use of the set is done
	views[0] = vkt.depthView;
	views[1] = vkt.normal.view;
	views[2] = vkt.velocity.view;
	views[3] = vkt.mbVelocity.view;
	views[4] = vkt.mbTile.view;
	views[5] = vkt.mbNeighbor.view;
	layouts[0] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	layouts[1] = layouts[2] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	for ( i = 3; i < VK_TEMPORAL_MB_BINDINGS; i++ ) {
		layouts[i] = VK_IMAGE_LAYOUT_GENERAL;
	}
	VK_TemporalWriteSet( set, vk_mbTypes, views, layouts, VK_TEMPORAL_MB_BINDINGS, -1 );

	if ( r_motionBlurFps->value > 0.0f ) {
		exposure = vkt.frameMsec > 0.0f ? r_motionBlurShutter->value * 1000.0f / ( r_motionBlurFps->value * vkt.frameMsec ) : 0.0f;
	} else {
		exposure = r_motionBlurShutter->value;
	}

	Com_Memset( &push, 0, sizeof( push ) );
	push.view = vkt.view;
	Com_Memcpy( push.rect, vkt.rect, sizeof( push.rect ) );
	push.params[0] = 0.5f * exposure;	// the blur reaches half of it on each side
	push.params[1] = Com_Clamp( 0.0f, 1.0f, r_motionBlurCamera->value );
	push.params[2] = (float)vkt.mbTileSize;
	push.tiles[0] = vkt.mbTilesX;
	push.tiles[1] = vkt.mbTilesY;

	qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.mbLayout, 0, 1, &set, 0, NULL );
	qvkCmdPushConstants( cmd, vkt.mbLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );

	// a workgroup a tile, over the framebuffer (the tiles start at its
	// corner; outside the views no blur)
	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.mbTilesPipeline );
	qvkCmdDispatch( cmd, vkt.mbTilesX, vkt.mbTilesY, 1 );
	VK_TemporalComputeBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.mbNeighborPipeline );
	qvkCmdDispatch( cmd, ( vkt.mbTilesX + VK_TEMPORAL_MB_NEIGHBOR_GROUP - 1 ) / VK_TEMPORAL_MB_NEIGHBOR_GROUP,
		( vkt.mbTilesY + VK_TEMPORAL_MB_NEIGHBOR_GROUP - 1 ) / VK_TEMPORAL_MB_NEIGHBOR_GROUP, 1 );

	vkt.mbRan = qtrue;
}

// whether this scene's depth of field passes run (r_dof, r_temporalShow 13)
static qboolean VK_TemporalDofs( void ) {
	return vkt.dofOn && vkt.dofSets[0] && vkt.rect[2] && vkt.rect[3];
}

/*
===============
VK_TemporalDof

In VK_TemporalResolve after the anti-aliasing, before the motion blur (the
depth image, the normals and the sharp scene readable: taa[current] when the
anti-aliasing ran, else the scene's color): the five passes of dof.comp, the
last writing dofResult, which the composite shows. The blur of a view
distance d is S (1 / F - 1 / d) pixels for the focus distance F, S from
r_dofAperture (the far distance's blur at F = VK_DOF_REFERENCE_FOCUS, in
percent of the primary view's height) times the zoom (a narrower field of
view than the game's default, up to 4). The focus eases toward its target with
the time constant r_dofFocusSpeed over the game time between the frames
(vkt.frameMsec), at once when the view has no last frame (a cut). Its
images were last read by the last frame's composite and this pass's focus
written by the last frame's: the barriers before the resolve's passes and at
the end of the last one's wait for them. Sets vkt.dofRan
===============
*/
static void VK_TemporalDof( VkCommandBuffer cmd ) {
	const VkDescriptorSet set = vkt.dofSets[vk.frameIndex];
	const int half = vkt.dofTileSize / 2;
	VkImageView views[VK_TEMPORAL_DOF_BINDINGS];
	VkImageLayout layouts[VK_TEMPORAL_DOF_BINDINGS];
	vkDofPush_t push;
	float zoom;
	int i;

	// the depth image's view can change between frames, and the sharp scene
	// with the anti-aliasing; this frame slot's last use of the set is done
	views[0] = vkt.depthView;
	views[1] = vkt.normal.view;
	views[2] = vkt.taaRan ? vkt.taa[vkt.current].view : vkt.color[vkt.colorIndex].view;
	views[3] = vkt.dofFocus.view;
	views[4] = vkt.dofHalf.view;
	views[5] = vkt.dofTile.view;
	views[6] = vkt.dofNeighbor.view;
	views[7] = vkt.dofFar.view;
	views[8] = vkt.dofNear.view;
	views[9] = vkt.dofResult.view;
	layouts[0] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	layouts[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	layouts[2] = vkt.taaRan ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	for ( i = VK_TEMPORAL_DOF_SAMPLED; i < VK_TEMPORAL_DOF_BINDINGS; i++ ) {
		layouts[i] = VK_IMAGE_LAYOUT_GENERAL;
	}
	VK_TemporalWriteSet( set, vk_dofTypes, views, layouts, VK_TEMPORAL_DOF_BINDINGS, -1 );

	zoom = Com_Clamp( 0.5f, 4.0f, fabsf( vkt.view.proj[1] ) / VK_DOF_PROJ_FOV80 );

	Com_Memset( &push, 0, sizeof( push ) );
	push.view = vkt.view;
	Com_Memcpy( push.rect, vkt.rect, sizeof( push.rect ) );
	push.lens[0] = r_dofAperture->value * 0.01f * vkt.view.rect[3] * VK_DOF_REFERENCE_FOCUS * zoom;
	push.lens[1] = (float)vkt.dofTileSize;
	push.lens[2] = r_dofFocus->value > 0.0f ? 1.0f / ( r_dofFocus->value > 8.0f ? r_dofFocus->value : 8.0f ) : 0.0f;
	if ( vkt.view.params[2] == 0.0f || r_dofFocusSpeed->value <= 0.0f ) {
		push.lens[3] = 1.0f;
	} else {
		push.lens[3] = 1.0f - expf( -vkt.frameMsec / ( 1000.0f * r_dofFocusSpeed->value ) );
	}
	push.params[0] = 0.02f * vkt.view.rect[3];
	push.params[2] = vkt.dofShow ? 1.0f : 0.0f;
	push.ints[0] = vkt.dofTilesX;
	push.ints[1] = vkt.dofTilesY;
	push.ints[2] = vk_dofSamples[Q_min( Q_max( r_dofQuality->integer, 0 ), 2 )];

	qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.dofLayout, 0, 1, &set, 0, NULL );
	qvkCmdPushConstants( cmd, vkt.dofLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );

	// the focus first: the barrier after it also waits for the anti-aliasing
	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.dofFocusPipeline );
	qvkCmdDispatch( cmd, 1, 1, 1 );
	VK_TemporalComputeBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

	// a workgroup a tile, over the half resolution image
	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.dofSetupPipeline );
	qvkCmdDispatch( cmd, vkt.dofTilesX, vkt.dofTilesY, 1 );
	VK_TemporalComputeBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.dofNeighborPipeline );
	qvkCmdDispatch( cmd, ( vkt.dofTilesX + VK_TEMPORAL_DOF_GROUP - 1 ) / VK_TEMPORAL_DOF_GROUP,
		( vkt.dofTilesY + VK_TEMPORAL_DOF_GROUP - 1 ) / VK_TEMPORAL_DOF_GROUP, 1 );
	VK_TemporalComputeBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

	// the views' half resolution texels
	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.dofGatherPipeline );
	qvkCmdDispatch( cmd, ( Q_min( vkt.dofTilesX * half, vkt.dofHalfWidth ) + VK_TEMPORAL_DOF_GROUP - 1 ) / VK_TEMPORAL_DOF_GROUP,
		( Q_min( vkt.dofTilesY * half, vkt.dofHalfHeight ) + VK_TEMPORAL_DOF_GROUP - 1 ) / VK_TEMPORAL_DOF_GROUP, 1 );
	VK_TemporalComputeBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

	// the views' pixels
	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.dofApplyPipeline );
	qvkCmdDispatch( cmd, ( vkt.rect[2] + VK_TEMPORAL_DOF_GROUP - 1 ) / VK_TEMPORAL_DOF_GROUP,
		( vkt.rect[3] + VK_TEMPORAL_DOF_GROUP - 1 ) / VK_TEMPORAL_DOF_GROUP, 1 );

	vkt.dofRan = qtrue;
}

/*
===============
VK_TemporalResolve

After the scene's last render pass, outside of a render pass: the depth
pyramid (levels 0 and 1 from the depth buffer, then the others) when a
reader needs it, then what resolves the scene, the depth of field's and the
motion blur's passes;
the depth image goes back to an attachment for the 2D drawing that follows. When the anti-aliasing is the
pyramid's only reader it makes level 0 itself in its pass (it has the view
distances of its pixels already; the pyramid's passes and barriers cost
about as much as a fifth of it), unless the views don't cover the
framebuffer (its dispatch covers the views only)
===============
*/
void VK_TemporalResolve( VkCommandBuffer cmd ) {
	const int cur = vkt.current;
	const VkDescriptorSet prepareSet = vkt.prepareSets[vk.frameIndex];
	const VkPipelineStageFlags depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	const VkPipelineStageFlags readers = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	const qboolean taa = VK_TemporalTaa();
	const qboolean blur = VK_TemporalBlurs();
	const qboolean dof = VK_TemporalDofs();
	qboolean pyramid = ( VK_TemporalReaders() & VK_TEMPORAL_READ_DEPTH ) != 0;
	VkImageMemoryBarrier barriers[6];
	VkMemoryBarrier memory;
	VkImageView views[VK_TEMPORAL_PREPARE_BINDINGS];
	VkImageLayout layouts[VK_TEMPORAL_PREPARE_BINDINGS];
	VkPipeline bound = VK_NULL_HANDLE;
	vkPreparePush_t prepare;
	vkHizPush_t hiz;
	int i, w, h, first, levels, count;

	vkt.taaRan = qfalse;
	vkt.mbRan = qfalse;
	vkt.dofRan = qfalse;
	if ( taa && !pyramid && ( vkt.rect[0] || vkt.rect[1] || vkt.rect[2] != vk.width || vkt.rect[3] != vk.height ) ) {
		pyramid = qtrue;
	}
	if ( !pyramid && !taa && !blur && !dof ) {
		// the scene's images to their readers, the depth image stays as it is
		VK_TemporalImageBarrier( &barriers[0], vkt.normal.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
		VK_TemporalImageBarrier( &barriers[1], vkt.velocity.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
		VK_TemporalImageBarrier( &barriers[2], vkt.color[vkt.colorIndex].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
		qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, readers, 0, 0, NULL, 0, NULL, 3, barriers );
		return;
	}
	// this frame's level 0 at least (the anti-aliasing's or the prepare
	// pass's); the motion blur and the depth of field alone make none (the
	// group may not be there)
	if ( pyramid || taa ) {
		vkt.hizSerial[cur] = vk.frameSerial;
	}

	VK_TemporalViewDepth();

	if ( pyramid ) {
		// the prepare pass's set: the depth image's view can change between
		// frames; this frame slot's last use of it is done
		views[0] = vkt.depthView;
		views[1] = vkt.normal.view;
		views[2] = vkt.hizLevel[cur][0];
		views[3] = vkt.hizLevel[cur][Q_min( 1, vkt.hizLevels - 1 )];
		layouts[0] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		layouts[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		layouts[2] = layouts[3] = VK_IMAGE_LAYOUT_GENERAL;
		VK_TemporalWriteSet( prepareSet, vk_prepareTypes, views, layouts, VK_TEMPORAL_PREPARE_BINDINGS, -1 );
	}

	// what the scene wrote, to its readers; this frame's pyramid was last
	// read (as the history) by the frame before, taa[cur] (the result two
	// frames ago) by that frame's composite and the last frame's resolve:
	// both written over; the motion blur's images (GENERAL) were last read
	// by the last frame's composite
	VK_TemporalImageBarrier( &barriers[0], vk.depthImage, vk.depthAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	VK_TemporalImageBarrier( &barriers[1], vkt.normal.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	VK_TemporalImageBarrier( &barriers[2], vkt.velocity.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	VK_TemporalImageBarrier( &barriers[3], vkt.color[vkt.colorIndex].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	count = 4;
	if ( pyramid || taa ) {
		VK_TemporalImageBarrier( &barriers[count++], vkt.hiz[cur].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT );
	}
	if ( taa ) {
		VK_TemporalImageBarrier( &barriers[count++], vkt.taa[cur].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT );
	}
	qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | depthStages | readers, readers,
		0, 0, NULL, 0, NULL, count, barriers );

	if ( pyramid ) {
		// levels 0 and 1, an invocation per texel of level 0 (8 x 8 a workgroup)
		levels = Q_min( 2, vkt.hizLevels );
		Com_Memset( &prepare, 0, sizeof( prepare ) );
		prepare.view = vkt.view;
		prepare.size[0] = vk.width;
		prepare.size[1] = vk.height;
		prepare.size[2] = vkt.hizWidth;
		prepare.size[3] = vkt.hizHeight;
		prepare.count = levels;
		w = vkt.hizWidth;
		h = vkt.hizHeight;
		qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.preparePipeline );
		qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.prepareLayout, 0, 1, &prepareSet, 0, NULL );
		qvkCmdPushConstants( cmd, vkt.prepareLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( prepare ), &prepare );
		qvkCmdDispatch( cmd, ( w + 7 ) / 8, ( h + 7 ) / 8, 1 );
		VK_TemporalComputeBarrier( cmd, vkt.numHizPasses ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : readers );

		// the other levels
		for ( i = 0; i < vkt.numHizPasses; i++ ) {
			const vkHizPass_t *pass = &vkt.hizPasses[i];
			VkPipeline pipeline = pass->tail ? vkt.hizTailPipeline : vkt.hizPipeline;

			if ( pipeline != bound ) {
				qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline );
				bound = pipeline;
			}
			Com_Memset( &hiz, 0, sizeof( hiz ) );
			hiz.size[0] = Q_max( vkt.hizWidth >> pass->src, 1 );
			hiz.size[1] = Q_max( vkt.hizHeight >> pass->src, 1 );
			hiz.count = pass->count;
			qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.hizLayout, 0, 1, &vkt.hizSets[cur][i], 0, NULL );
			qvkCmdPushConstants( cmd, vkt.hizLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( hiz ), &hiz );
			if ( pass->tail ) {
				qvkCmdDispatch( cmd, 1, 1, 1 );
			} else {
				// an invocation per texel of the first level it makes
				first = pass->src + 1;
				w = Q_max( vkt.hizWidth >> first, 1 );
				h = Q_max( vkt.hizHeight >> first, 1 );
				qvkCmdDispatch( cmd, ( w + 7 ) / 8, ( h + 7 ) / 8, 1 );
			}
			VK_TemporalComputeBarrier( cmd, i + 1 < vkt.numHizPasses ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : readers );
		}
	}

	if ( taa ) {
		VK_TemporalConsumers( cmd, !pyramid );
	}
	if ( dof ) {
		VK_TemporalDof( cmd );
	}
	if ( blur ) {
		VK_TemporalMotionBlur( cmd );
	}

	// the 2D drawing and the next scene draw with the depth image again; what
	// the anti-aliasing, the depth of field and the motion blur wrote to the
	// composite and to the next frame's (the history, level 0 of the pyramid,
	// the focus)
	VK_TemporalImageBarrier( &barriers[0], vk.depthImage, vk.depthAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
	Com_Memset( &memory, 0, sizeof( memory ) );
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	qvkCmdPipelineBarrier( cmd, readers, depthStages | ( taa || blur || dof ? readers : 0 ), 0, taa || blur || dof ? 1 : 0, &memory, 0, NULL, 1, barriers );
}

/*
===============
VK_TemporalComposite

In vk.renderPass, after VK_TemporalResolve: the scene into the frame's color
target, over the rectangle of its views (or what r_temporalShow shows): the
anti-aliased scene when the resolve made it, sharpened (r_taaSharpen), or
the depth of field's result when its passes ran (r_dof: what it blurred not
sharpened),
and blurred along the motion blur's vectors when its passes ran
(r_motionBlur; not over r_temporalShow 13's view of the depth of field).
The caller rebinds what it draws with next
===============
*/
void VK_TemporalComposite( VkCommandBuffer cmd ) {
	vkCompositePush_t push;
	VkViewport viewport;
	VkRect2D scissor;

	viewport.x = 0;
	viewport.y = 0;
	viewport.width = vk.width;
	viewport.height = vk.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	scissor.offset.x = vkt.rect[0];
	scissor.offset.y = vkt.rect[1];
	scissor.extent.width = vkt.rect[2];
	scissor.extent.height = vkt.rect[3];
	if ( !scissor.extent.width || !scissor.extent.height ) {
		return;
	}

	Com_Memset( &push, 0, sizeof( push ) );
	push.view = vkt.view;
	push.mode = vkt.showMode;
	push.mip = r_temporalShowMip->integer;
	push.scale = r_temporalShowScale->value;
	push.sharpen = vkt.taaRan ? r_taaSharpen->value : 0.0f;
	Com_Memcpy( push.rect, vkt.rect, sizeof( push.rect ) );
	VK_OutputExpansion( push.hdr );
	push.mb[0] = vkt.mbTileSize;
	push.mb[1] = vk_mbPairs[Q_min( Q_max( r_motionBlurQuality->integer, 0 ), 2 )];
	push.mb[2] = vkt.dofRan ? 1 : 0;

	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkt.mbRan && vkt.mbBlur && !( vkt.dofRan && vkt.dofShow )
		? vkt.compositeBlurPipeline : vkt.compositePipeline );
	qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkt.compositeLayout, 0, 1,
		vkt.dofRan ? &vkt.compositeDofSets[vkt.current] : vkt.taaRan ? &vkt.compositeTaaSets[vkt.current] : &vkt.compositeSets[vkt.current],
		0, NULL );
	qvkCmdPushConstants( cmd, vkt.compositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( push ), &push );
	qvkCmdSetViewport( cmd, 0, 1, &viewport );
	qvkCmdSetScissor( cmd, 0, 1, &scissor );
	qvkCmdDraw( cmd, 3, 1, 0, 0 );
}
