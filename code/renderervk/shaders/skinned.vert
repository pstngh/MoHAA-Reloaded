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

// skinned.vert -- vertex shader of the skeletal models skinned and lit on
// the GPU (tr_skin.c, vk_draw with drawGeometry_t::isSkinned)
//
// It does what RB_SkelMesh, RB_Light_Real (rgbGen lightingSpherical) and
// RB_CalcEnvironmentTexCoords(2) (tcGen environment, environmentmodel) did
// on the CPU, with the same floating point operations in the same order
// (precise: no fused multiply-adds the C code doesn't have), so positions,
// normals, colors and texture coordinates come out the same to the bit;
// divisions get one correction step, which makes them correctly rounded
// like the CPU's in all but very rare cases. The outputs are generic.vert's.
//
// The vertex comes from the skin data (gl_VertexIndex: the draw's
// vertexOffset is the surface's first vertex record), what changes per
// frame from the frame data, starting at the draw record gl_InstanceIndex
// (the draw's firstInstance). Layouts: see vkSkinDraw_t in vk.h.

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

#ifdef TEMPORAL
// the last frame's frame data (r_temporal): the entity's bones and morph
// weights of the last frame are there, at the words before its view origin
// says (VK_SKIN_PREV_*, vk.h)
layout(std430, set = 2, binding = 3) readonly buffer SkinPrevFrame {
	uint	prevFrame[];
};
#endif

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_st0;
layout(location = 2) out vec2 v_st1;
layout(location = 3) out vec2 v_fogClip;
layout(location = 4) out vec3 v_position;
// bindless textures: unit 0's slot, above renderCount in the draw record
// (vk_draw); unused without them
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

// lighttype_t
const uint LIGHT_POINT = 0u;
const uint LIGHT_DIRECTIONAL = 1u;
const uint LIGHT_SPOT = 2u;
const uint LIGHT_SPOT_FAST = 3u;

const uint NONE = 0xFFFFFFFFu;

// the texture coordinates (vk.h, VK_SKIN_ENV_*)
const uint ENV_SHIFT = 30u;
const uint ENV_MAPPED = 1u;
const uint ENV_MAPPED2 = 2u;

float skinFloat( uint i ) {
	return uintBitsToFloat( skin[i] );
}

float frameFloat( uint i ) {
	return uintBitsToFloat( frame[i] );
}

vec3 frameVec3( uint i ) {
	return vec3( frameFloat( i ), frameFloat( i + 1u ), frameFloat( i + 2u ) );
}

// a / b, rounded like the CPU's division: the reciprocal of the GPU is
// within an ulp, one correction step with the exact residual makes the
// quotient correctly rounded but for very rare operands
float divide( float a, float b ) {
	precise float r = 1.0 / b;
	precise float q = a * r;
	precise float e = fma( -b, q, a );
	precise float c = fma( e, r, q );
	return c;
}

// c / 255 correctly rounded, the value of an R8G8B8A8_UNORM vertex color
// (the CPU path's colors): exact for every byte with one correction step
float unorm8( uint c ) {
	const float r = uintBitsToFloat( 0x3B808081u );	// 1 / 255
	precise float q = float( c ) * r;
	precise float e = fma( -255.0, q, float( c ) );
	precise float d = fma( e, r, q );
	return d;
}

vec4 unpackColor( uint c ) {
	return vec4( unorm8( c & 255u ), unorm8( ( c >> 8 ) & 255u ), unorm8( ( c >> 16 ) & 255u ), unorm8( c >> 24 ) );
}

// DotProduct of q_shared.h
float dot3( vec3 a, vec3 b ) {
	precise float d = a.x * b.x + a.y * b.y + a.z * b.z;
	return d;
}

// Q_rsqrt of q_math.c
float qRsqrt( float number ) {
	precise float x2 = number * 0.5;
	precise float y = intBitsToFloat( 0x5f3759df - ( floatBitsToInt( number ) >> 1 ) );
	precise float t = x2 * y * y;
	y = y * ( 1.5 - t );
	return y;
}

// the first word of a bone in the frame data (skelBoneCache_t: offset,
// then the rows of the rotation)
uint bone( uint draw, uint index ) {
	uint remap = frame[draw + 1u];

	if ( remap != NONE ) {
		index = skin[remap + index];
	}
	return frame[draw] + index * 16u;
}

#ifdef TEMPORAL
// the position of the vertex with the last frame's bones and morph weights
// (in the last frame's frame data from prevBones and prevMorphs), as main
// computes it with this frame's
vec3 prevPosition( uint draw, uint vert, uint prevBones, uint prevMorphs, float scale ) {
	uint counts = skin[vert + 5u];
	uint numWeights = counts & 0xFFFFu;
	uint numMorphs = counts >> 16;
	uint p = vert + skin[vert + 6u];
	uint remap = frame[draw + 1u];

	precise vec3 totalmorph = vec3( 0.0 );
	for ( uint m = 0u; m < numMorphs; m++, p += 4u ) {
		int weight = prevMorphs != NONE ? int( prevFrame[prevMorphs + skin[p]] ) : 0;

		if ( weight != 0 ) {
			totalmorph = totalmorph + vec3( skinFloat( p + 1u ), skinFloat( p + 2u ), skinFloat( p + 3u ) ) * float( weight );
		}
	}

	precise vec3 xyz = vec3( 0.0 );
	for ( uint w = 0u; w < numWeights; w++, p += 5u ) {
		uint index = skin[p];
		if ( remap != NONE ) {
			index = skin[remap + index];
		}
		uint b = prevBones + index * 16u;
		vec3 offset = vec3( uintBitsToFloat( prevFrame[b] ), uintBitsToFloat( prevFrame[b + 1u] ), uintBitsToFloat( prevFrame[b + 2u] ) );
		vec3 row0 = vec3( uintBitsToFloat( prevFrame[b + 4u] ), uintBitsToFloat( prevFrame[b + 5u] ), uintBitsToFloat( prevFrame[b + 6u] ) );
		vec3 row1 = vec3( uintBitsToFloat( prevFrame[b + 8u] ), uintBitsToFloat( prevFrame[b + 9u] ), uintBitsToFloat( prevFrame[b + 10u] ) );
		vec3 row2 = vec3( uintBitsToFloat( prevFrame[b + 12u] ), uintBitsToFloat( prevFrame[b + 13u] ), uintBitsToFloat( prevFrame[b + 14u] ) );
		precise vec3 point = vec3( skinFloat( p + 2u ), skinFloat( p + 3u ), skinFloat( p + 4u ) );

		if ( w == 0u && prevMorphs != NONE ) {
			point = totalmorph + point;
		}
		xyz = xyz + ( ( point.x * row0 + point.y * row1 + point.z * row2 ) + offset ) * skinFloat( p + 1u );
	}
	return xyz * scale;
}
#endif

// RB_Light_Real: the lights of the entity's sphere, in its space
vec4 lightColor( uint list, vec3 xyz, vec3 normal ) {
	uint numLights = frame[list];
	uint ambient = frame[list + 1u];
	precise vec3 colorout = vec3( 0.0 );

	for ( uint j = 0u; j < numLights; j++ ) {
		uint l = list + 2u + j * 16u;
		uint type = frame[l];
		vec3 color = frameVec3( l + 1u );
		vec3 origin = frameVec3( l + 4u );
		vec3 direction = frameVec3( l + 7u );
		precise vec3 v;
		precise float fDot, fDistSquared, fProjSquared, fMaxIntensity, proj;

		if ( type == LIGHT_DIRECTIONAL ) {
			fDot = dot3( direction, normal );
			if ( fDot > 0.0 ) {
				colorout = colorout + color * fDot;
			}
		} else if ( type == LIGHT_SPOT ) {
			fDot = dot3( direction, normal );
			if ( fDot > 0.0 ) {
				v = origin - xyz;
				proj = dot3( v, direction );
				fProjSquared = proj * proj;
				fDistSquared = dot3( v, v );
				fMaxIntensity = ( frameFloat( l + 10u ) - divide( fDistSquared, fProjSquared ) ) * frameFloat( l + 11u );
				if ( fMaxIntensity > 0.0 ) {
					fDot = divide( fDot, fDistSquared );
					if ( fMaxIntensity < 1.0 ) {
						fDot = fDot * fMaxIntensity;
					}
					colorout = colorout + color * fDot;
				}
			}
		} else if ( type == LIGHT_SPOT_FAST ) {
			fDot = dot3( direction, normal );
			if ( fDot > 0.0 ) {
				v = origin - xyz;
				fDistSquared = dot3( v, v );
				colorout = colorout + color * divide( fDot, fDistSquared );
			}
		} else {
			v = origin - xyz;
			fDot = dot3( v, normal );
			if ( fDot > 0.0 ) {
				fDistSquared = dot3( v, v );
				colorout = colorout + color * divide( fDot, fDistSquared );
			}
		}
	}

	ivec3 level = ivec3( ambient & 255u, ( ambient >> 8 ) & 255u, ( ambient >> 16 ) & 255u );
	uvec3 c = uvec3( clamp( ivec3( colorout ) + level, 0, 255 ) );
	return vec4( unorm8( c.r ), unorm8( c.g ), unorm8( c.b ), 1.0 );
}

void main() {
	uint draw = uint( gl_InstanceIndex ) * 8u;
	uint word6 = frame[draw + 6u];
	uint renderCount = word6 & 0xFFFFu;	// VK_SKIN_NONE collapses nothing either way: no surface has that many vertexes
	v_tex = uvec2( word6 >> 16, 0u );
	uint firstVertex = frame[draw + 7u] & ( ( 1u << ENV_SHIFT ) - 1u );
	uint env = frame[draw + 7u] >> ENV_SHIFT;
	uint v = uint( gl_VertexIndex ) - firstVertex;

	// RB_SkelMesh's collapse map at a lower level of detail
	while ( v >= renderCount ) {
		v = skin[( firstVertex + v ) * 8u + 7u];
	}
	uint vert = ( firstVertex + v ) * 8u;
	uint morphs = frame[draw + 2u];
	uint lights = frame[draw + 3u];
	float scale = frameFloat( draw + 4u );
	uint counts = skin[vert + 5u];
	uint numWeights = counts & 0xFFFFu;
	uint numMorphs = counts >> 16;
	uint p = vert + skin[vert + 6u];
	vec3 normalIn = vec3( skinFloat( vert ), skinFloat( vert + 1u ), skinFloat( vert + 2u ) );

	// SkelMorphGetXyz
	precise vec3 totalmorph = vec3( 0.0 );
	for ( uint m = 0u; m < numMorphs; m++, p += 4u ) {
		int weight = morphs != NONE ? int( frame[morphs + skin[p]] ) : 0;

		if ( weight != 0 ) {
			totalmorph = totalmorph + vec3( skinFloat( p + 1u ), skinFloat( p + 2u ), skinFloat( p + 3u ) ) * float( weight );
		}
	}

	// SkelVertGetNormal with the first weight's bone, SkelWeightGetXyz and
	// SkelWeightMorphGetXyz (the first weight of an entity with morphs)
	precise vec3 normal = vec3( 0.0 );
	precise vec3 xyz = vec3( 0.0 );
	for ( uint w = 0u; w < numWeights; w++, p += 5u ) {
		uint b = bone( draw, skin[p] );
		vec3 offset = frameVec3( b );
		vec3 row0 = frameVec3( b + 4u );
		vec3 row1 = frameVec3( b + 8u );
		vec3 row2 = frameVec3( b + 12u );
		precise vec3 point = vec3( skinFloat( p + 2u ), skinFloat( p + 3u ), skinFloat( p + 4u ) );
		precise vec3 t;

		if ( w == 0u ) {
			normal = normalIn.x * row0 + normalIn.y * row1 + normalIn.z * row2;
			if ( morphs != NONE ) {
				point = totalmorph + point;
			}
		}
		t = ( point.x * row0 + point.y * row1 + point.z * row2 ) + offset;
		xyz = xyz + t * skinFloat( p + 1u );
	}
	xyz = xyz * scale;

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

	v_color = lights != NONE ? lightColor( lights, xyz, normal ) : unpackColor( frame[draw + 5u] );
	if ( env == ENV_MAPPED || env == ENV_MAPPED2 ) {
		// RB_CalcEnvironmentTexCoords, RB_CalcEnvironmentTexCoords2: the view
		// origin in the entity's space is in front of its bones
		precise vec3 viewer = frameVec3( frame[draw] - 4u ) - xyz;
		precise vec3 reflected;
		precise float d;
		precise vec2 st;

		viewer = viewer * qRsqrt( dot3( viewer, viewer ) );
		d = dot3( normal, viewer );
		if ( env == ENV_MAPPED ) {
			reflected = ( normal * 2.0 ) * d - viewer;
		} else if ( d > 0.0 ) {
			reflected = viewer;
		} else {
			d = d * -2.0;
			reflected = normal * d + viewer;
		}
		st.x = 0.5 + reflected.y * 0.5;
		st.y = 0.5 - reflected.z * 0.5;
		v_st0 = st;
	} else {
		v_st0 = vec2( skinFloat( vert + 3u ), skinFloat( vert + 4u ) );
	}
	v_st1 = vec2( 0.0 );
	v_position = xyz;

#ifdef TEMPORAL
	v_lodBias = pc.jitter.w;
	if ( !TEMPORAL_NORMALS ) {
		return;
	}
	if ( TEMPORAL_MOTION ) {
		// the words before the view origin: the last frame's bones and morph
		// weights in its frame data, VK_SKIN_NONE without them
		uint prevBones = frame[frame[draw] - 8u];
		uint prevMorphs = frame[frame[draw] - 7u];
		vec3 prevXyz = prevBones != NONE ? prevPosition( draw, vert, prevBones, prevMorphs, scale ) : xyz;

		v_currClip = gl_Position - vec4( pc.jitter.xy * gl_Position.w, 0.0, 0.0 );
		v_prevClip = pc.prevMvp * vec4( prevXyz, 1.0 );
	}
	v_viewPos = vec3( dot( pc.mvRow0, vec4( xyz, 1.0 ) ), dot( pc.mvRow1, vec4( xyz, 1.0 ) ), dot( pc.eyeZ, vec4( xyz, 1.0 ) ) );
	v_viewNormal = vec3( dot( pc.mvRow0.xyz, normal ), dot( pc.mvRow1.xyz, normal ), dot( pc.eyeZ.xyz, normal ) );
	v_class = pc.jitter.z;
#endif
}
