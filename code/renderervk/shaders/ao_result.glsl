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

// ao_result.glsl -- what the ambient occlusion's blurred visibility becomes
// in the scene (r_ao, ao_common.glsl): shared by the compute passes and
// ao_apply.frag, which don't share their bindings; the including shader
// declares the push constant block (vkAoPush_t) first.

// the sky and what is farther at AoFar. The view distances are 32-bit
// floats: in half floats (XeGTAO's) a wall close to the eye or seen at a
// grazing angle stepped from pixel to pixel, each step a ledge that
// occluded the next: the wall went dark in stripes
float AoFar() {
	return min( pc.view.depth.w, 60000.0 );
}

bool AoIsFar( float d ) {
	return d >= AoFar() * 0.999;
}

// Jimenez et al. 2016, the multi-bounce fit
float AoMultiBounce( float v, float albedo ) {
	float a = 2.0404 * albedo - 0.3324;
	float b = -4.7951 * albedo + 0.6417;
	float c = 2.7552 * albedo + 0.6903;

	return max( v, ( ( v * a + b ) * v + c ) * v );
}

// what ao_apply.frag blends into the scene for the blurred visibility v at
// view distance sd: v to the power r_aoStrength, the multi-bounce fit, in
// the scene's gamma-encoded values (2.2: the game draws them so), undone
// through the fog: the scene is C f + fog (1 - f), the occlusion belongs on
// C alone; with a the factor, rgb = fog (1 - f)(1 - a), blended ONE,
// SRC_ALPHA: dst a + rgb
vec4 AoResult( float v, float sd ) {
	float d = abs( sd );

	if ( AoIsFar( d ) ) {
		return vec4( 0.0, 0.0, 0.0, 1.0 );
	}
	// in linear light, then the scene's encoding
	v = pow( clamp( v, 0.0, 1.0 ), pc.ao.w );
	if ( pc.ao2.x > 0.0 ) {
		v = AoMultiBounce( v, pc.ao2.x );
	}
	float a = pow( clamp( v, 0.0, 1.0 ), 1.0 / 2.2 );
	vec3 fog = vec3( 0.0 );

	if ( pc.fogColor.w != 0.0 ) {
		float f = clamp( d * pc.fogParams.x + pc.fogParams.y, 0.0, 1.0 );

		fog = pc.fogColor.rgb * ( 1.0 - f ) * ( 1.0 - a );
	}
	return vec4( fog, a );
}
