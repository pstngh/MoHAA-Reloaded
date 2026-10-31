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

// generic.frag -- the GL1 fixed-function fragment pipeline
//
// The variant is selected with specialization constants, so each pipeline
// only runs the stages its state needs (see vk_pipeline.c).

#version 450
#ifdef TEMPORAL
#extension GL_GOOGLE_include_directive : require
#include "temporal_common.glsl"
#endif

// texture env of each unit, texEnv_t values from vk.h; 0 disables the unit
layout(constant_id = 0) const int TEX_ENV0 = 0;
layout(constant_id = 1) const int TEX_ENV1 = 0;
// the texture of the unit has no alpha channel: it samples as 1 and
// TEXENV_REPLACE keeps the previous alpha. The image view swizzle gives the
// 1, or, without imageViewFormatSwizzle (portability subset), the shader:
// the pipelines then have it set with every texture env
layout(constant_id = 2) const bool TEX0_RGB = false;
layout(constant_id = 3) const bool TEX1_RGB = false;
// alpha test: 0 off, 1 keep alpha > ref, 2 keep alpha < ref, 3 keep alpha >= ref
layout(constant_id = 4) const int ALPHA_FUNC = 0;
layout(constant_id = 5) const float ALPHA_REF = 0.0;
// linear fog with the parameters of the push constants
layout(constant_id = 6) const bool FOG = false;
// discard on the clip plane side (when the vertex shader can't use
// gl_ClipDistance)
layout(constant_id = 7) const bool CLIP_DISCARD = false;
// no blending: the alpha written is 1 (the framebuffer has no destination
// alpha, see vk_pipeline.c)
layout(constant_id = 8) const bool ALPHA_ONE = false;
// dynamic lights added to the texels of a lightmap (vk_set_lightmap_dlights):
// 0 none, 1 the texture of unit 0, 2 that of unit 1
layout(constant_id = 9) const int DLIGHT_UNIT = 0;
// no light from behind the plane of the triangle (r_dlightBacks 0)
layout(constant_id = 10) const bool DLIGHT_BACKS = false;

// the fragment stage range of the push constants (vkPushConstants_t in vk.c)
layout(push_constant) uniform PushConstants {
	layout(offset = 96) vec4	fogColor;
	// -1 / (end - start), end / (end - start), as Mesa's optimized fog
	// parameters
	vec2	fogParams;
	// the lights the draw adds: those of a mask over the 32 lights of
	// dlights[] from dlightBase, or with DLIGHT_COUNT in dlightBase the
	// first dlightMask lights from its other bits (vk_set_lightmap_dlights)
	uint	dlightBase;
	uint	dlightMask;
} pc;

const uint DLIGHT_COUNT = 0x80000000u;

// vkDlightGPU_t
struct Dlight {
	vec4	originInvRadius;	// origin in object space, 1 / radius
	vec4	colorScale;			// added at the origin in lightmap bytes, then times w
};

layout(std430, set = 2, binding = 0) readonly buffer Dlights {
	Dlight	dlights[];
};

// texEnv_t
const int TEXENV_MODULATE = 1;
const int TEXENV_REPLACE = 2;
const int TEXENV_DECAL = 3;
const int TEXENV_ADD = 4;
const int TEXENV_COMBINE_INTERPOLATE = 5;

#ifdef BINDLESS
// bindless textures (vk.bindless): every texture slot in one set, the
// draw's units' slots from the vertex stage (firstInstance or the draw's
// record), the same for every vertex of the draw
layout(set = 0, binding = 0) uniform sampler2D u_textures[4096];
layout(location = 11) flat in uvec2 v_tex;
#define TEX0 u_textures[v_tex.x]
#define TEX1 u_textures[v_tex.y]
#else
layout(set = 0, binding = 0) uniform sampler2D u_texture0;
layout(set = 1, binding = 0) uniform sampler2D u_texture1;
#define TEX0 u_texture0
#define TEX1 u_texture1
#endif

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_st0;
layout(location = 2) in vec2 v_st1;
layout(location = 3) in vec2 v_fogClip;
layout(location = 4) in vec3 v_position;

layout(location = 0) out vec4 out_color;

#ifdef TEMPORAL
// the temporal path (r_temporal, vk_temporal.c): the scene's normals and
// motion vectors, written by the pipelines that write depth without
// blending (the others mask these attachments and skip this), the motion
// vectors only by those that draw what moves
layout(location = 5) in vec4 v_currClip;
layout(location = 6) in vec4 v_prevClip;
layout(location = 7) in vec3 v_viewNormal;
layout(location = 8) in vec3 v_viewPos;
layout(location = 9) flat in float v_class;
// the anti-aliasing's texture LOD bias (r_taaLodBias): the history adds the
// detail of a finer mip level without its shimmer
layout(location = 10) flat in float v_lodBias;

// this frame's position minus the last frame's, in units of the viewport
// (x right, y down)
layout(location = 1) out vec2 out_velocity;
// rg: the view space normal, octahedral; b (10 bits): TEMPORAL_FLAG_*;
// a (2 bits): the class (TEMPORAL_CLASS_*)
layout(location = 2) out vec4 out_normal;

// the pipeline writes the normals buffer (vk_pipeline.c), and the motion
// vectors
layout(constant_id = 11) const bool TEMPORAL_NORMALS = true;
layout(constant_id = 12) const bool TEMPORAL_MOTION = true;
// the color's alpha is the scene's reactive share (vk_pipeline.c): 0 for the
// draws without blending; a blended effect's (VK_PIPE_REACTIVE, added up
// as coverage: a + d ( 1 - a )) its alpha (1), its brightest channel (2, it adds), one minus
// its darkest (3, it multiplies), or with an additive blend by its alpha
// the brightest channel of its color times its alpha, which it then is
// (4: the pipeline blends ONE instead of SRC_ALPHA); the anti-aliasing
// takes less history there (taa_resolve.comp)
layout(constant_id = 13) const int TEMPORAL_REACTIVE = 0;

void TemporalReactive( vec4 color ) {
	if ( ALPHA_ONE ) {
		out_color.a = 0.0;
	} else if ( TEMPORAL_REACTIVE == 4 ) {
		out_color.rgb = color.rgb * color.a;
		out_color.a = clamp( max( out_color.r, max( out_color.g, out_color.b ) ), 0.0, 1.0 );
	} else if ( TEMPORAL_REACTIVE == 2 ) {
		out_color.a = clamp( max( color.r, max( color.g, color.b ) ), 0.0, 1.0 );
	} else if ( TEMPORAL_REACTIVE == 3 ) {
		out_color.a = clamp( 1.0 - min( color.r, min( color.g, color.b ) ), 0.0, 1.0 );
	}
}

void TemporalOutputs() {
	if ( !TEMPORAL_NORMALS ) {
		return;
	}

	vec3 n = v_viewNormal;
	float len = dot( n, n );
	uint flags = 0u;

	if ( TEMPORAL_MOTION ) {
		vec2 velocity = vec2( 0.0 );

		if ( v_prevClip.w > 1e-6 ) {
			velocity = ( v_currClip.xy / v_currClip.w - v_prevClip.xy / v_prevClip.w ) * vec2( 0.5, -0.5 );
		}
		out_velocity = velocity;
		flags |= TEMPORAL_FLAG_MOTION;
	}

	if ( len > 1e-12 ) {
		n *= inversesqrt( len );
		// towards the eye, for two-sided surfaces
		if ( dot( n, v_viewPos ) > 0.0 ) {
			n = -n;
		}
		out_normal = vec4( TemporalOctEncode( n ), float( flags | TEMPORAL_FLAG_NORMAL ) / 1023.0, v_class / 3.0 );
	} else {
		// derived from the depth by the readers (TemporalDerivedNormal)
		out_normal = vec4( 0.5, 0.5, float( flags ) / 1023.0, v_class / 3.0 );
	}
}

#define SAMPLE( tex, st ) texture( tex, st, v_lodBias )
#else
#define SAMPLE( tex, st ) texture( tex, st )
#endif

// GL1's R_DlightSample, per pixel on the filtered lightmap texel: each light
// adds color * ( 1 - d / radius )^2 at distance d, truncated to a lightmap
// byte, then shifted (colorScale.w: tr.overbrightMult / 255), and the sum is
// divided by its largest component when that is above 1, truncated to a
// byte again. normal: the side of the triangle a light must be on for
// DLIGHT_BACKS
vec3 AddDlight( vec3 sum, Dlight dl, vec3 normal ) {
	vec3 dir = dl.originInvRadius.xyz - v_position;
	float d = sqrt( dot( dir, dir ) ) * dl.originInvRadius.w;

	if ( d <= 1.0 && ( !DLIGHT_BACKS || dot( dir, normal ) >= 0.0 ) ) {
		sum += floor( ( 1.0 - d ) * ( 1.0 - d ) * dl.colorScale.rgb ) * dl.colorScale.w;
	}
	return sum;
}

vec3 AddDlights( vec3 lightmap, vec3 normal ) {
	vec3 sum = lightmap;

	if ( ( pc.dlightBase & DLIGHT_COUNT ) != 0u ) {
		uint base = pc.dlightBase & ~DLIGHT_COUNT;

		for ( uint i = 0u; i < pc.dlightMask; i++ ) {
			sum = AddDlight( sum, dlights[base + i], normal );
		}
	} else {
		uint mask = pc.dlightMask;

		while ( mask != 0u ) {
			sum = AddDlight( sum, dlights[pc.dlightBase + uint( findLSB( mask ) )], normal );
			mask &= mask - 1u;
		}
	}

	float m = max( sum.r, max( sum.g, sum.b ) );
	return m > 1.0 ? floor( sum * ( 255.0 / m ) ) * ( 1.0 / 255.0 ) : sum;
}

vec4 TexEnv( int env, bool rgbTexture, vec4 previous, vec4 texel ) {
	if ( env == TEXENV_REPLACE ) {
		return rgbTexture ? vec4( texel.rgb, previous.a ) : texel;
	}
	if ( env == TEXENV_DECAL ) {
		return vec4( mix( previous.rgb, texel.rgb, texel.a ), previous.a );
	}
	if ( env == TEXENV_ADD ) {
		return vec4( min( previous.rgb + texel.rgb, 1.0 ), previous.a * texel.a );
	}
	if ( env == TEXENV_COMBINE_INTERPOLATE ) {
		return vec4( v_color.rgb * v_color.a + texel.rgb * ( 1.0 - v_color.a ), previous.a * texel.a );
	}
	// TEXENV_MODULATE, and TEXENV_COMBINE4 which no shader produces
	return previous * texel;
}

void main() {
	vec4 color = v_color;
	vec3 normal = vec3( 0.0 );

	// the plane of the triangle, with derivatives in uniform control flow:
	// dFdx and dFdy of the position span it, and their cross product points
	// away from the eye (framebuffer y goes down); a back face (clockwise,
	// as BSP faces are seen from their front) faces the eye
	if ( DLIGHT_BACKS ) {
		normal = cross( dFdx( v_position ), dFdy( v_position ) );
		if ( !gl_FrontFacing ) {
			normal = -normal;
		}
	}

	if ( CLIP_DISCARD && v_fogClip.y < 0.0 ) {
		discard;
	}

	if ( TEX_ENV0 != 0 ) {
		vec4 texel = SAMPLE( TEX0, v_st0 );

		if ( TEX0_RGB ) {
			texel.a = 1.0;
		}
		if ( DLIGHT_UNIT == 1 ) {
			texel.rgb = AddDlights( texel.rgb, normal );
		}
		color = TexEnv( TEX_ENV0, TEX0_RGB, color, texel );
	}
	if ( TEX_ENV1 != 0 ) {
		vec4 texel = SAMPLE( TEX1, v_st1 );

		if ( TEX1_RGB ) {
			texel.a = 1.0;
		}
		if ( DLIGHT_UNIT == 2 ) {
			texel.rgb = AddDlights( texel.rgb, normal );
		}
		color = TexEnv( TEX_ENV1, TEX1_RGB, color, texel );
	}

	if ( ALPHA_FUNC == 1 ) {
		if ( !( color.a > ALPHA_REF ) ) {
			discard;
		}
	} else if ( ALPHA_FUNC == 2 ) {
		if ( !( color.a < ALPHA_REF ) ) {
			discard;
		}
	} else if ( ALPHA_FUNC == 3 ) {
		if ( !( color.a >= ALPHA_REF ) ) {
			discard;
		}
	}

	// Mesa's fixed-function fog (st_nir_lower_fog): a single multiply-add
	// for the factor, then fogColor * ( 1 - f ) + color * f; alpha unchanged
	if ( FOG ) {
		float f = clamp( fma( v_fogClip.x, pc.fogParams.x, pc.fogParams.y ), 0.0, 1.0 );

		color.rgb = fma( color.rgb, vec3( f ), pc.fogColor.rgb * ( 1.0 - f ) );
	}

	out_color = ALPHA_ONE ? vec4( color.rgb, 1.0 ) : color;
#ifdef TEMPORAL
	TemporalReactive( color );
	TemporalOutputs();
#endif
}
