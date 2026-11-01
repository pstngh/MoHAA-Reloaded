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

// ao_common.glsl -- what the passes of the ambient occlusion share (r_ao,
// VK_TemporalAo in vk_temporal.c): one descriptor set and one push constant
// block for all of them, each pass uses some of the bindings.
//  - ao_prefilter.comp: the depth buffer to view distances, 5 levels
//  - gtao_main.comp: the occlusion (GTAO), noisy
//  - ao_temporal.comp: blended with the last frames' result and blurred
//    (AoBlur) into the factor the apply pass multiplies the scene with
//    (AoResult, ao_apply.frag), or the first of two blurs
//  - ao_denoise.comp: the second of two blurs
//  - ao_apply.frag: the half resolution's blurred visibility to the
//    framebuffer's pixels, blended into the scene (ao_upsample.comp does
//    the same into the result image, only for r_temporalShow 9's view)
// View distances are positive, the view weapon's negative (it is in its own
// depth range and only occludes itself), the sky at the far distance.
// The occlusion, its blending over frames and its blur work on a grid
// (AO_HALF, always true since Plan 49): the framebuffer's pixels, or one texel a 2 x 2 of
// them, which is its top-left pixel (its view distance and its position:
// a plane stays a plane on the grid; the nearest and the farthest of the
// four on alternate texels made it a zigzag that occluded itself).
// Positions, the radius and the motion are in the framebuffer's pixels
// either way.

#extension GL_GOOGLE_include_directive : require
#include "temporal_common.glsl"

#define AO_LEVELS	5	// of the view distances (VK_TEMPORAL_AO_LEVELS)

// the pipelines are made with it true (vk_temporal.c); false, the full
// resolution of Plan 44, is kept in the code but no longer used
layout( constant_id = 2 ) const bool AO_HALF = false;
const int AO_SHIFT = AO_HALF ? 1 : 0;		// framebuffer pixels to the grid
const float AO_SCALE = AO_HALF ? 2.0 : 1.0;	// the grid's texel in framebuffer pixels

// the framebuffer position of a grid texel's pixel's center
vec2 AoGridPixel( ivec2 t ) {
	return vec2( t ) * AO_SCALE + 0.5;
}

layout( set = 0, binding = 0 ) uniform sampler2D u_depth;		// the depth buffer
layout( set = 0, binding = 1 ) uniform sampler2D u_normal;
layout( set = 0, binding = 2 ) uniform sampler2D u_velocity;
layout( set = 0, binding = 3 ) uniform sampler2D u_aoDepth;		// this frame's view distances of the grid (every level without AO_HALF)
layout( set = 0, binding = 4 ) uniform sampler2D u_prevAoDepth;	// the last frame's
layout( set = 0, binding = 5 ) uniform sampler2D u_raw;			// gtao_main.comp's result: one frame's visibility
layout( set = 0, binding = 6 ) uniform sampler2D u_history;		// the last frame's accumulated visibility, frames
layout( set = 0, binding = 7, r32f ) uniform writeonly image2D o_depth0;
layout( set = 0, binding = 8, r32f ) uniform writeonly image2D o_depth1;
layout( set = 0, binding = 9, r32f ) uniform writeonly image2D o_depth2;
layout( set = 0, binding = 10, r32f ) uniform writeonly image2D o_depth3;
layout( set = 0, binding = 11, r32f ) uniform writeonly image2D o_depth4;
layout( set = 0, binding = 12, r8 ) uniform writeonly image2D o_raw;
layout( set = 0, binding = 13, rg16f ) uniform writeonly image2D o_history;	// this frame's accumulated visibility, frames
layout( set = 0, binding = 14, rg16f ) uniform writeonly image2D o_blur;		// the first blur's result with two blurs
layout( set = 0, binding = 15, rgba8 ) uniform writeonly image2D o_result;	// what ao_apply.frag blends (8 bits: less than a step of the frame)
layout( set = 0, binding = 16 ) uniform sampler2D u_aoLevels;		// with AO_HALF the view distances of the framebuffer, every level (without, u_aoDepth has them)
layout( set = 0, binding = 17 ) uniform sampler2D u_blur;			// o_blur, sampled
layout( set = 0, binding = 18, rg16f ) uniform writeonly image2D o_blur2;	// with AO_HALF: the last blur's result, which ao_upsample.comp reads
layout( set = 0, binding = 19 ) uniform sampler2D u_blur2;			// o_blur2, sampled
layout( set = 0, binding = 20, r32f ) uniform writeonly image2D o_grid;	// with AO_HALF: the grid's view distances (u_aoDepth)
layout( set = 0, binding = 21, rgba16f ) uniform writeonly image2D o_gridMotion;	// the grid's motion (ao_prefilter.comp)
layout( set = 0, binding = 22 ) uniform sampler2D u_gridMotion;		// what ao_temporal.comp moves its history by

// the grid's motion, baked by the prefilter so the passes after it read
// nothing of the scene (the motion vectors and the normals stay the
// draws'): the texel's motion in units of the viewport, the view distance
// its surface had in the last frame, and AO_MOTION_*: 0 nothing to take
// history from (behind the camera), 1 a surface or the weapon (the
// camera's motion, or none), 2 a draw's motion vector (a looser depth
// match, less history)
#define AO_MOTION_NONE		0.0
#define AO_MOTION_CAMERA	1.0
#define AO_MOTION_DRAW		2.0

// vkAoPush_t
layout( push_constant ) uniform Push {
	TemporalView	view;
	vec4	ao;			// the radius (world units), the falloff's share of it, the thin occluder compensation, r_aoStrength (a power)
	vec4	ao2;		// the multi-bounce albedo (0: none), 1: the history is the frame before's, the most frames blended, the blur's center weight
	vec4	fogColor;	// rgb, 1 when the view is fogged
	vec4	fogParams;	// the fog factor's scale and offset (of the view distance), 1: the draws' normals, 1: the last view distances are the frame before's
	ivec4	ints;		// slices, steps per side, the noise's frame index, the blur pass (AO_PASS_*)
} pc;

// the blurs (pc.ints.w): ao_temporal.comp's to o_blur (two blurs) or to the
// result (one), ao_denoise.comp's o_blur to the result
const int AO_PASS_HISTORY_TO_BLUR = 0;
const int AO_PASS_HISTORY_TO_RESULT = 1;
const int AO_PASS_BLUR_TO_RESULT = 2;

// the 3 x 3 around a texel p of level 0 in 4 gathers of one component:
// corners at p, p + (1, 0), p + (0, 1), p + (1, 1) (in texels). Gather order
// is (x0, y1), (x1, y1), (x1, y0), (x0, y0): g[0] = (L, C, T, TL),
// g[1] = (C, R, TR, T), g[2] = (BL, B, C, L), g[3] = (B, BR, R, C)
#define AO_GATHER3X3( tex, p, comp, g ) { \
	vec2 inv_ = 1.0 / vec2( textureSize( tex, 0 ) ); \
	g[0] = textureGather( tex, vec2( p ) * inv_, comp ); \
	g[1] = textureGather( tex, vec2( p + ivec2( 1, 0 ) ) * inv_, comp ); \
	g[2] = textureGather( tex, vec2( p + ivec2( 0, 1 ) ) * inv_, comp ); \
	g[3] = textureGather( tex, vec2( p + ivec2( 1, 1 ) ) * inv_, comp ); }

// the grid's view rectangle: the views' texels
ivec2 AoGridLo() {
	return pc.view.rect.xy >> AO_SHIFT;
}

ivec2 AoGridHi() {
	return ( pc.view.rect.xy + pc.view.rect.zw - 1 ) >> AO_SHIFT;
}

// AoFar, AoIsFar, AoMultiBounce, AoResult (ao_apply.frag shares them)
#include "ao_result.glsl"

// the view distance of a pixel from its depth and its normal's class
float AoDistance( float wz, float a ) {
	int cls = int( a * 3.0 + 0.5 );		// TemporalClass
	bool weapon = cls == TEMPORAL_CLASS_WEAPON;
	float d = TemporalViewDistance( pc.view, wz, weapon );

	if ( cls == TEMPORAL_CLASS_NONE || cls == TEMPORAL_CLASS_SKY ) {
		return AoFar();
	}
	return weapon ? -max( d, 1e-4 ) : min( d, AoFar() );
}

// XeGTAO's edges of a pixel of view distance z to its neighbours (left,
// right, top, bottom): 1 where the neighbour's distance continues the
// surface (the slope across the pixel taken out), 0 where it's another
// surface; continuous. XeGTAO packs them in 2 bits each: a camera moved by a
// ten-thousandth of a unit (a player pushing into a wall) rounds every
// depth anew, edges near a step flipped by a third, and the blur's weights
// with them: the occlusion wobbled in bands
vec4 AoEdges( float z, float l, float r, float t, float b ) {
	vec4 e = vec4( l, r, t, b ) - z;
	float slopeLR = ( e.y - e.x ) * 0.5;
	float slopeTB = ( e.w - e.z ) * 0.5;
	vec4 adjusted = e + vec4( slopeLR, -slopeLR, slopeTB, -slopeTB );

	e = min( abs( e ), abs( adjusted ) );
	// a texel of the half resolution grid is two pixels across
	return clamp( 1.25 - e / ( abs( z ) * ( 0.011 * AO_SCALE ) + 1e-4 ), 0.0, 1.0 );
}

// XeGTAO's blur (Intel, MIT license) of the pixel C: the 3 x 3 around it,
// each neighbour weighted by the depth edges between them both ways
// (AoEdges, from the view distances of the 3 x 3 and the 4 pixels two away,
// LL, RR, TT, BB), so nothing crosses from one surface to another; the center
// weighs pc.ao2.w (the first of two blurs a fifth of it), times 1 + a
// quarter of the frames blended past the first: a converged pixel keeps its
// detail. v: TL, T, TR, L, C, R, BL, B, BR; z: the same, then LL, RR, TT, BB
float AoBlur( float v[9], float z[13], float frames, bool final ) {
	vec4 eC = AoEdges( z[4], z[3], z[5], z[1], z[7] );
	vec4 eL = AoEdges( z[3], z[9], z[4], z[0], z[6] );
	vec4 eR = AoEdges( z[5], z[4], z[10], z[2], z[8] );
	vec4 eT = AoEdges( z[1], z[0], z[2], z[11], z[4] );
	vec4 eB = AoEdges( z[7], z[6], z[8], z[4], z[12] );
	const float diag = 0.85 * 0.5;

	// both ways
	eC *= vec4( eL.y, eR.x, eT.w, eB.z );
	float wTL = diag * ( eC.x * eL.z + eC.z * eT.x );
	float wTR = diag * ( eC.z * eT.y + eC.y * eR.z );
	float wBL = diag * ( eC.w * eB.x + eC.x * eL.w );
	float wBR = diag * ( eC.y * eR.w + eC.w * eB.y );
	float center = pc.ao2.w * ( final ? 1.0 : 0.2 ) * ( 1.0 + 0.25 * max( frames - 1.0, 0.0 ) );
	float sum = v[4] * center + v[3] * eC.x + v[5] * eC.y + v[1] * eC.z + v[7] * eC.w
		+ v[0] * wTL + v[2] * wTR + v[6] * wBL + v[8] * wBR;

	return sum / ( center + dot( eC, vec4( 1.0 ) ) + wTL + wTR + wBL + wBR );
}

