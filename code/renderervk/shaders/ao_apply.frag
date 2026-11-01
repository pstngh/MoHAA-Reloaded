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

// ao_apply.frag -- the ambient occlusion into the scene (r_ao,
// VK_TemporalAo): drawn in the scene's render pass after its opaque
// surfaces and before what is blended over them (the effects in front of a
// corner aren't darkened), blended ONE, SRC_ALPHA into the color only:
// dst a + rgb. The half resolution grid's blurred visibility
// (ao_temporal.comp, ao_denoise.comp) is upsampled here, as
// ao_upsample.comp does it (that pass runs only for r_temporalShow 9 since
// Plan 50: writing the result out and reading it back cost more than this
// pass's extra work): each pixel's visibility from the 2 x 2 texels around
// it, bilinear weights times how close each texel's view distance is to
// the pixel's own, so the occlusion of a surface doesn't spread over the
// one in front of it or behind; where no texel is of the pixel's surface,
// the nearest in view distance. The pixel's view distance is level 0 of
// the prefilter's (ao_prefilter.comp writes AoDistance of every pixel
// there): the depth buffer and the normals are this render pass's
// attachments, which a draw here can't read. Then the result (AoResult,
// the factor and the fog's part) as the upsampling pass makes it.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "temporal_common.glsl"

layout( set = 0, binding = 0 ) uniform sampler2D u_dist;	// the pixels' view distances: the prefilter's level 0
layout( set = 0, binding = 1 ) uniform sampler2D u_grid;	// the grid's (a texel a 2 x 2 pixels)
layout( set = 0, binding = 2 ) uniform sampler2D u_vis;		// the grid's blurred visibility: the last blur (and its frames)

// vkAoPush_t, as the compute passes' (ao_common.glsl)
layout( push_constant ) uniform Push {
	TemporalView	view;
	vec4	ao;			// the radius (world units), the falloff's share of it, the thin occluder compensation, r_aoStrength (a power)
	vec4	ao2;		// the multi-bounce albedo (0: none), 1: the history is the frame before's, the most frames blended, the blur's center weight
	vec4	fogColor;	// rgb, 1 when the view is fogged
	vec4	fogParams;	// the fog factor's scale and offset (of the view distance), 1: the draws' normals, 1: the last view distances are the frame before's
	ivec4	ints;		// slices, steps per side, the noise's frame index, the blur pass (AO_PASS_*)
} pc;

#include "ao_result.glsl"

layout( location = 0 ) out vec4 out_color;

void main() {
	ivec2 p = ivec2( gl_FragCoord.xy );
	ivec2 glo = pc.view.rect.xy >> 1;
	ivec2 ghi = ( pc.view.rect.xy + pc.view.rect.zw - 1 ) >> 1;
	float sd = texelFetch( u_dist, p, 0 ).r;
	float d = abs( sd );
	bool weapon = sd < 0.0;

	// nothing to change: no blend, no write
	if ( AoIsFar( d ) ) {
		discard;
	}

	// the texels around the pixel (a texel is at its top-left pixel: the
	// pixel itself, or between two or four of them)
	vec2 g = vec2( p ) * 0.5;
	ivec2 b = ivec2( floor( g ) );
	vec2 f = g - vec2( b );
	float tolerance = 0.03 * d + 1.0;
	float sum = 0.0, wsum = 0.0, nearestV = 1.0, nearestE = 3.4e38;
	int x, y;

	for ( y = 0; y < 2; y++ ) {
		for ( x = 0; x < 2; x++ ) {
			ivec2 t = clamp( b + ivec2( x, y ), glo, ghi );
			float td = texelFetch( u_grid, t, 0 ).r;
			float v = texelFetch( u_vis, t, 0 ).r;
			float e = ( td < 0.0 ) == weapon ? abs( abs( td ) - d ) : 3.0e38;
			float w = ( x == 1 ? f.x : 1.0 - f.x ) * ( y == 1 ? f.y : 1.0 - f.y ) * max( 1.0 - e / tolerance, 0.0 );

			sum += w * v;
			wsum += w;
			if ( e < nearestE ) {
				nearestE = e;
				nearestV = v;
			}
		}
	}

	vec4 ao = AoResult( wsum > 1e-4 ? sum / wsum : nearestV, sd );

	// nothing to change (most of the view): no blend, no write
	if ( ao.a >= 254.5 / 255.0 ) {
		discard;
	}
	out_color = ao;
}
