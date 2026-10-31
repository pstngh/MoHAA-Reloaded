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

// effects.vert -- vertex shader of the static geometry draws whose shader
// computes something per vertex (tr_staticgeom.c, VK_PIPE_EFFECTS)
//
// It does what RB_DeformTessGeometry, ComputeTexCoords and ComputeColors
// do on the CPU for the cases RB_StaticFx* in tr_shade_calc.c accept (from
// JayRewd's b26d8a67), with the same floating point operations in the same
// order (precise: no fused multiply-adds the C code doesn't have). Where
// the C code works in double precision the result is computed exactly
// (sums and products that are exact in double) or with float-float
// arithmetic. Divisions and square roots get one correction step, which
// makes them correctly rounded like the CPU's in all but very rare cases.
// The outputs are generic.vert's.
//
// The streams are generic.vert's with unit 0 reading the texture
// coordinates and unit 1 the lightmap coordinates, plus the normals; the
// color stream is read per vertex, or ignored when the record has the
// draw's constant color (no binding has a stride of 0, which a portability
// subset device reads per instance: from firstInstance, the record). The
// draw's record (the layout is in tr_shade_calc.c, STATIC GEOMETRY
// EFFECTS) is in the frame data from word gl_InstanceIndex * 8 (the draw's
// firstInstance); the wave tables are in the skin data.

#version 450

layout(push_constant) uniform PushConstants {
	mat4	mvp;
	vec4	eyeZ;
	vec4	clipPlane;
#ifdef TEMPORAL
	// generic.vert's
	layout(offset = 128) mat4	prevMvp;
	vec4	mvRow0;
	vec4	mvRow1;
	vec4	jitter;
#endif
} pc;

layout(std430, set = 2, binding = 1) readonly buffer SkinData {
	uint	skin[];
};

layout(std430, set = 2, binding = 2) readonly buffer SkinFrame {
	uint	frame[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_st0;
layout(location = 3) in vec2 in_st1;
layout(location = 4) in vec3 in_normal;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_st0;
layout(location = 2) out vec2 v_st1;
layout(location = 3) out vec2 v_fogClip;
layout(location = 4) out vec3 v_position;
// bindless textures: the units' slots from the record (FX_TEX); unused
// without them
layout(location = 11) flat out uvec2 v_tex;
#ifdef TEMPORAL
// generic.vert's
layout(location = 5) out vec4 v_currClip;
layout(location = 6) out vec4 v_prevClip;
layout(location = 7) out vec3 v_viewNormal;
layout(location = 8) out vec3 v_viewPos;
layout(location = 9) flat out float v_class;
// the anti-aliasing's texture LOD bias (r_taaLodBias), for every pipeline
layout(location = 10) flat out float v_lodBias;
#endif

#ifdef CLIP_DISTANCE
out float gl_ClipDistance[1];
#endif

invariant gl_Position;

#ifdef TEMPORAL
// the temporal path: only the pipelines that write the normals buffer
// (vk_pipeline.c) compute what it needs, the motion vectors' only those
// that draw what moves (generic.frag)
layout(constant_id = 11) const bool TEMPORAL_NORMALS = true;
layout(constant_id = 12) const bool TEMPORAL_MOTION = true;
#endif

// record layout, in words from the record's first
const uint FX_FLAGS = 0u;
const uint FX_TABLES = 1u;
const uint FX_LIGHTS = 2u;
const uint FX_VIEW = 4u;
const uint FX_OVERBRIGHT = 7u;
const uint FX_EYE = 8u;
const uint FX_DIST = 11u;
const uint FX_DEFORMS = 16u;
const uint FX_UNITS = 40u;
const uint FX_UNIT_SIZE = 40u;
const uint FX_COLOR = 120u;
const uint FX_TEX = 124u;		// bindless textures: unit 0's slot | unit 1's << 16

// flags
const uint FLAG_OVERBRIGHT = 1u << 17;
const uint FLAG_LIGHTING = 1u << 10;
const uint FLAG_CONSTANT_COLOR = 1u << 11;

// deforms
const uint DEFORM_WAVE_CONST = 1u;
const uint DEFORM_WAVE = 2u;
const uint DEFORM_FLAP_S = 3u;
const uint DEFORM_FLAP_T = 4u;
const uint DEFORM_BULGE = 5u;
const uint DEFORM_MOVE = 6u;

// texture coordinate sources
const uint TC_TEXTURE = 1u;
const uint TC_LIGHTMAP = 2u;
const uint TC_IDENTITY = 3u;
const uint TC_ENVIRONMENT = 4u;
const uint TC_ENVIRONMENT2 = 5u;
const uint TC_VECTOR = 6u;

// texture coordinate modifiers
const uint TMOD_ADD = 1u;
const uint TMOD_SCALE = 2u;
const uint TMOD_TRANSFORM = 3u;
const uint TMOD_TURB = 4u;

// alpha
const uint ALPHA_DIST_FADE = 1u;
const uint ALPHA_ONE_MINUS_DIST_FADE = 2u;

const uint FUNCTABLE_MASK = 1023u;

uint rec;

float recFloat( uint i ) {
	return uintBitsToFloat( frame[rec + i] );
}

vec3 recVec3( uint i ) {
	return vec3( recFloat( i ), recFloat( i + 1u ), recFloat( i + 2u ) );
}

float frameFloat( uint i ) {
	return uintBitsToFloat( frame[i] );
}

float table( uint func, int index ) {
	return uintBitsToFloat( skin[frame[rec + FX_TABLES] + func * 1024u + ( uint( index ) & FUNCTABLE_MASK )] );
}

// DotProduct of q_shared.h
float dot3( vec3 a, vec3 b ) {
	precise float d = a.x * b.x + a.y * b.y + a.z * b.z;
	return d;
}

// a / b correctly rounded but for very rare operands (skinned.vert)
float divide( float a, float b ) {
	precise float r = 1.0 / b;
	precise float q = a * r;
	precise float e = fma( -b, q, a );
	precise float c = fma( e, r, q );
	return c;
}

// sqrt( a ) correctly rounded but for very rare operands: what
// (float)sqrt( (double)a ) gives
float sqrtCr( float a ) {
	precise float s = sqrt( a );
	precise float e = fma( -s, s, a );
	precise float c = fma( e, divide( 0.5, s ), s );
	return a > 0.0 ? c : s;
}

// c / 255 correctly rounded, the value of an R8G8B8A8_UNORM vertex color
float unorm8( uint c ) {
	const float r = uintBitsToFloat( 0x3B808081u );	// 1 / 255
	precise float q = float( c ) * r;
	precise float e = fma( -255.0, q, float( c ) );
	precise float d = fma( e, r, q );
	return d;
}

uint byte8( float unorm ) {
	return uint( unorm * 255.0 + 0.5 );
}

// (int)( ( a + b ) * 1024 ) with a + b and the product in double, as the C
// code's expressions with a double constant give: the sum of two floats
// exactly (two-sum), scaled by 1024 exactly, truncated toward zero
int truncSum1024( float a, float b ) {
	precise float s = a + b;
	precise float bb = s - a;
	precise float e = ( a - ( s - bb ) ) + ( b - bb );
	precise float hi = s * 1024.0;
	precise float lo = e * 1024.0;
	float t = trunc( hi );
	if ( t == hi ) {
		if ( hi > 0.0 && lo < 0.0 ) {
			t -= 1.0;
		} else if ( hi < 0.0 && lo > 0.0 ) {
			t += 1.0;
		}
	}
	return int( t );
}

// (unsigned char)( x * 255.0 ) for 0 <= x <= 1 with the product in double
// (exact there): the float product plus its rounding error, truncated
uint truncTimes255( float x ) {
	precise float p = x * 255.0;
	precise float e = fma( x, 255.0, -p );
	float t = floor( p );
	if ( t == p && e < 0.0 ) {
		t -= 1.0;
	}
	return uint( t ) & 255u;
}

// RB_DeformTessGeometry for the deforms RB_StaticFx accepts
void deform( inout vec3 xyz, vec3 normal, vec2 st ) {
	uint flags = frame[rec + FX_FLAGS];
	uint numDeforms = flags & 3u;

	for ( uint i = 0u; i < numDeforms; i++ ) {
		uint d = FX_DEFORMS + i * 8u;
		uint type = frame[rec + d];
		precise vec3 offset;
		precise float scale;

		if ( type == DEFORM_WAVE_CONST ) {
			// RB_CalcDeformVertexes, frequency 0
			scale = recFloat( d + 1u );
			offset = normal * scale;
			xyz = xyz + offset;
		} else if ( type == DEFORM_WAVE ) {
			// WAVEVALUE( table, base, amplitude, phase + off, frequency );
			// d + 5 is tess.shaderTime * frequency
			precise float off = ( xyz.x + xyz.y + xyz.z ) * recFloat( d + 6u );
			precise float arg = ( ( recFloat( d + 4u ) + off ) + recFloat( d + 5u ) ) * 1024.0;
			scale = recFloat( d + 2u ) + table( frame[rec + d + 1u], int( arg ) ) * recFloat( d + 3u );
			offset = normal * scale;
			xyz = xyz + offset;
		} else if ( type == DEFORM_FLAP_S || type == DEFORM_FLAP_T ) {
			// RB_CalcFlapVertexes
			precise float minV = recFloat( d + 2u );
			precise float maxV = recFloat( d + 3u );
			precise float vertexScale = ( maxV - minV ) * ( type == DEFORM_FLAP_S ? st.x : st.y ) + minV;
			scale = recFloat( d + 1u );
			offset = vec3( scale * vertexScale ) * normal;
			xyz = xyz + offset;
		} else if ( type == DEFORM_BULGE ) {
			// RB_CalcBulgeVertexes
			precise float arg = 1024.0 * ( st.x * recFloat( d + 2u ) + recFloat( d + 1u ) );
			scale = table( 0u, int( arg ) ) * recFloat( d + 3u );
			offset = normal * scale;
			xyz = xyz + offset;
		} else if ( type == DEFORM_MOVE ) {
			offset = recVec3( d + 1u );
			xyz = xyz + offset;
		}
	}
}

#ifdef TEMPORAL
// deform() at the last frame's game time: the same operations on the values
// RB_FxDeformMotion keeps in the words each deform leaves free (wave's time
// * frequency at 7, flap's scale and bulge's phase at 4, move's offset at 4
// to 6), so a deform that didn't change gives the same position, no motion
vec3 deformPrev( vec3 position, vec3 normal, vec2 st ) {
	uint flags = frame[rec + FX_FLAGS];
	uint numDeforms = flags & 3u;
	precise vec3 xyz = position;

	for ( uint i = 0u; i < numDeforms; i++ ) {
		uint d = FX_DEFORMS + i * 8u;
		uint type = frame[rec + d];
		precise vec3 offset;
		precise float scale;

		if ( type == DEFORM_WAVE_CONST ) {
			scale = recFloat( d + 1u );
			offset = normal * scale;
			xyz = xyz + offset;
		} else if ( type == DEFORM_WAVE ) {
			precise float off = ( xyz.x + xyz.y + xyz.z ) * recFloat( d + 6u );
			precise float arg = ( ( recFloat( d + 4u ) + off ) + recFloat( d + 7u ) ) * 1024.0;
			scale = recFloat( d + 2u ) + table( frame[rec + d + 1u], int( arg ) ) * recFloat( d + 3u );
			offset = normal * scale;
			xyz = xyz + offset;
		} else if ( type == DEFORM_FLAP_S || type == DEFORM_FLAP_T ) {
			precise float minV = recFloat( d + 2u );
			precise float maxV = recFloat( d + 3u );
			precise float vertexScale = ( maxV - minV ) * ( type == DEFORM_FLAP_S ? st.x : st.y ) + minV;
			scale = recFloat( d + 4u );
			offset = vec3( scale * vertexScale ) * normal;
			xyz = xyz + offset;
		} else if ( type == DEFORM_BULGE ) {
			precise float arg = 1024.0 * ( st.x * recFloat( d + 2u ) + recFloat( d + 4u ) );
			scale = table( 0u, int( arg ) ) * recFloat( d + 3u );
			offset = normal * scale;
			xyz = xyz + offset;
		} else if ( type == DEFORM_MOVE ) {
			offset = recVec3( d + 4u );
			xyz = xyz + offset;
		}
	}
	return xyz;
}
#endif

// Q_rsqrt of q_math.c
float qRsqrt( float number ) {
	precise float x2 = number * 0.5;
	precise float y = intBitsToFloat( 0x5f3759df - ( floatBitsToInt( number ) >> 1 ) );
	precise float t = x2 * y * y;
	y = y * ( 1.5 - t );
	return y;
}

// ComputeTexCoords for a bundle
vec2 texCoords( uint unit, vec3 xyz, vec3 normal ) {
	uint u = FX_UNITS + unit * FX_UNIT_SIZE;
	uint mode = frame[rec + u];
	uint numMods = frame[rec + u + 1u];
	precise vec2 st;

	if ( mode == TC_TEXTURE ) {
		st = in_st0;
	} else if ( mode == TC_LIGHTMAP ) {
		st = in_st1;
	} else if ( mode == TC_ENVIRONMENT || mode == TC_ENVIRONMENT2 ) {
		// RB_CalcEnvironmentTexCoords, RB_CalcEnvironmentTexCoords2
		precise vec3 viewer = recVec3( FX_VIEW ) - xyz;
		precise vec3 reflected;
		precise float d;

		viewer = viewer * qRsqrt( dot3( viewer, viewer ) );
		d = dot3( normal, viewer );
		if ( mode == TC_ENVIRONMENT ) {
			reflected = ( normal * 2.0 ) * d - viewer;
		} else if ( d > 0.0 ) {
			reflected = viewer;
		} else {
			d = d * -2.0;
			reflected = normal * d + viewer;
		}
		st.x = 0.5 + reflected.y * 0.5;
		st.y = 0.5 - reflected.z * 0.5;
	} else if ( mode == TC_VECTOR ) {
		st.x = dot3( xyz, recVec3( u + 2u ) );
		st.y = dot3( xyz, recVec3( u + 5u ) );
	} else {
		st = vec2( 0.0 );
	}

	for ( uint m = 0u; m < numMods; m++ ) {
		uint o = u + 8u + m * 8u;
		uint type = frame[rec + o];

		if ( type == TMOD_ADD ) {
			st.x = st.x + recFloat( o + 1u );
			st.y = st.y + recFloat( o + 2u );
		} else if ( type == TMOD_SCALE ) {
			st.x = st.x * recFloat( o + 1u );
			st.y = st.y * recFloat( o + 2u );
		} else if ( type == TMOD_TRANSFORM ) {
			// RB_CalcTransformTexCoords
			precise float s = st.x;
			precise float t = st.y;
			st.x = s * recFloat( o + 1u ) + t * recFloat( o + 2u ) + recFloat( o + 3u );
			st.y = s * recFloat( o + 4u ) + t * recFloat( o + 5u ) + recFloat( o + 6u );
		} else if ( type == TMOD_TURB ) {
			// RB_CalcTurbulentTexCoords: ( v * 1.0/128 * 0.125 + now ) *
			// FUNCTABLE_SIZE in double, v / 1024 is exact
			precise float now = recFloat( o + 1u );
			precise float amplitude = recFloat( o + 2u );
			precise float sx = ( xyz.x + xyz.z ) * ( 1.0 / 1024.0 );
			precise float sy = xyz.y * ( 1.0 / 1024.0 );
			st.x = st.x + table( 0u, truncSum1024( sx, now ) ) * amplitude;
			st.y = st.y + table( 0u, truncSum1024( sy, now ) ) * amplitude;
		}
	}
	return st;
}

// float-float product of ( ah, al ) and ( bh, bl )
vec2 ffMul( vec2 a, vec2 b ) {
	precise float h = a.x * b.x;
	precise float l = fma( a.x, b.x, -h ) + ( a.x * b.y + a.y * b.x );
	precise float s = h + l;
	precise float e = l - ( s - h );
	return vec2( s, e );
}

// ComputeColors' useSpecialLighting loop of a static model: the vertex
// colors plus the dynamic lights with N.L and an inverse square falloff
vec3 lighting( vec3 xyz, vec3 normal ) {
	uint list = frame[rec + FX_LIGHTS];
	uint numLights = frame[list];
	precise vec3 colorout = vec3( float( byte8( in_color.r ) ), float( byte8( in_color.g ) ), float( byte8( in_color.b ) ) );

	for ( uint j = 0u; j < numLights; j++ ) {
		uint l = list + 8u + j * 8u;
		precise vec3 diff = vec3( frameFloat( l ), frameFloat( l + 1u ), frameFloat( l + 2u ) ) - xyz;
		precise float dotv = dot3( diff, normal );

		if ( dotv >= 0.0 ) {
			// ooLen = 1.0 / VectorLengthSquared( diff ), then
			// dot * ( 7500.0 * radius * ooLen * sqrt( ooLen ) ) in double;
			// 7500.0 * radius is l + 3 and l + 4 exactly
			float ooLen = divide( 1.0, dot3( diff, diff ) );
			precise float sh = sqrtCr( ooLen );
			precise float sl = divide( fma( -sh, sh, ooLen ), 2.0 * sh );
			vec2 a = vec2( frameFloat( l + 3u ), frameFloat( l + 4u ) );
			vec2 p = ffMul( ffMul( a, vec2( ooLen, 0.0 ) ), vec2( sh, sl ) );
			p = ffMul( vec2( dotv, 0.0 ), p );
			precise float oo = p.x + p.y;
			precise vec3 color = vec3( frameFloat( l + 5u ), frameFloat( l + 6u ), frameFloat( l + 7u ) );
			colorout = color * oo + colorout;
		}
	}

	ivec3 c = ivec3( colorout );
	uint flags = frame[rec + FX_FLAGS];
	if ( ( flags & FLAG_OVERBRIGHT ) != 0u ) {
		precise float mult = recFloat( FX_OVERBRIGHT );
		c = ivec3( vec3( c ) * mult );
	}
	if ( c.r > 255 || c.g > 255 || c.b > 255 ) {
		precise float t = divide( 255.0, float( max( c.r, max( c.g, c.b ) ) ) );
		c = ivec3( vec3( c ) * t );
	}
	uvec3 b = uvec3( c ) & 255u;
	return vec3( unorm8( b.r ), unorm8( b.g ), unorm8( b.b ) );
}

// ComputeColors' distFade and oneMinusDistFade
float distFade( uint mode, vec3 xyz ) {
	precise vec3 org = xyz - recVec3( FX_EYE );
	precise float len = sqrtCr( dot3( org, org ) );
	len = divide( len - recFloat( FX_DIST ), recFloat( FX_DIST + 1u ) );

	uint alpha;
	if ( mode == ALPHA_DIST_FADE ) {
		if ( len < 0.0 ) {
			alpha = 255u;
		} else if ( len > 1.0 ) {
			alpha = 0u;
		} else {
			// ( 1.0 - len ) * 255.0 in double: 1 - len exactly as a
			// float-float, then each part times 255 exactly
			precise float hi = 1.0 - len;
			precise float lo = ( 1.0 - hi ) - len;
			precise float p = hi * 255.0;
			precise float e = fma( hi, 255.0, -p ) + lo * 255.0;
			float t = floor( p );
			precise float r = ( p - t ) + e;
			if ( r < 0.0 ) {
				t -= 1.0;
			} else if ( r >= 1.0 ) {
				t += 1.0;
			}
			alpha = uint( t ) & 255u;
		}
	} else {
		if ( len < 0.0 ) {
			alpha = 0u;
		} else if ( len > 1.0 ) {
			alpha = 255u;
		} else {
			alpha = truncTimes255( len );
		}
	}
	return unorm8( alpha );
}

void main() {
	rec = uint( gl_InstanceIndex ) * 8u;
	uint flags = frame[rec + FX_FLAGS];
	v_tex = uvec2( frame[rec + FX_TEX] & 0xFFFFu, frame[rec + FX_TEX] >> 16 );
	precise vec3 xyz = in_position;
	vec3 normal = in_normal;

	deform( xyz, normal, in_st0 );

	// generic.vert
	vec4 pos = pc.mvp[0] * xyz.x;
	pos = fma( pc.mvp[1], vec4( xyz.y ), pos );
	pos = fma( pc.mvp[2], vec4( xyz.z ), pos );
	gl_Position = pos + pc.mvp[3];

	float eyeZ = pc.eyeZ.x * xyz.x;
	eyeZ = fma( pc.eyeZ.y, xyz.y, eyeZ );
	eyeZ = fma( pc.eyeZ.z, xyz.z, eyeZ );
	v_fogClip.x = abs( eyeZ + pc.eyeZ.w );

	v_fogClip.y = dot( pc.clipPlane, vec4( xyz, 1.0 ) );
#ifdef CLIP_DISTANCE
	gl_ClipDistance[0] = v_fogClip.y;
#endif

	// the vertex colors, or the draw's constant color
	vec4 color = in_color;
	if ( ( flags & FLAG_CONSTANT_COLOR ) != 0u ) {
		color = vec4( recVec3( FX_COLOR ), recFloat( FX_COLOR + 3u ) );
	}
	if ( ( flags & FLAG_LIGHTING ) != 0u ) {
		color.rgb = lighting( xyz, normal );
	}
	uint alphaMode = ( flags >> 8 ) & 3u;
	if ( alphaMode != 0u ) {
		color.a = distFade( alphaMode, xyz );
	}
	v_color = color;

	v_st0 = texCoords( 0u, xyz, normal );
	v_st1 = texCoords( 1u, xyz, normal );
	v_position = xyz;

#ifdef TEMPORAL
	v_lodBias = pc.jitter.w;
	if ( !TEMPORAL_NORMALS ) {
		return;
	}
	if ( TEMPORAL_MOTION ) {
		// where the deforms had the vertex in the last frame (deformPrev),
		// through the last frame's transform: the entity's motion, or the
		// camera's, and the deforms' own
		v_currClip = gl_Position - vec4( pc.jitter.xy * gl_Position.w, 0.0, 0.0 );
		v_prevClip = pc.prevMvp * vec4( deformPrev( in_position, normal, in_st0 ), 1.0 );
	}
	v_viewPos = vec3( dot( pc.mvRow0, vec4( xyz, 1.0 ) ), dot( pc.mvRow1, vec4( xyz, 1.0 ) ), dot( pc.eyeZ, vec4( xyz, 1.0 ) ) );
	v_viewNormal = vec3( dot( pc.mvRow0.xyz, normal ), dot( pc.mvRow1.xyz, normal ), dot( pc.eyeZ.xyz, normal ) );
	v_class = pc.jitter.z;
#endif
}
