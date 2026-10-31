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

// generic.vert -- vertex shader for every draw of the fixed-function
// emulation (see generic.frag)
//
// Every pipeline provides all four attributes: draws without a color array
// read one RGBA float color with a binding stride of 0, and texture units
// without coordinates read a dummy value the fragment shader ignores.
//
// compile_shaders.sh builds two variants: the default one, and one with
// CLIP_DISTANCE defined that clips against the user clip plane with
// gl_ClipDistance (pipelines with a clip plane, when the device supports
// shaderClipDistance; otherwise generic.frag discards). With TEMPORAL
// defined (both again) it is the temporal path's (r_temporal, vk_temporal.c):
// it also reads normals and writes what the motion vectors and the normals
// buffer need.

#version 450

// the vertex stage range of the push constants (vkPushConstants_t in vk.c)
layout(push_constant) uniform PushConstants {
	// projection * modelview, computed like Mesa's _ModelProjectMatrix; the
	// projection is remapped to z in [0, w] unless VK_EXT_depth_clip_control
	// gives GL clip space; y is flipped by the viewport
	mat4	mvp;
	// row 2 of the modelview: eye space z, for the fog distance
	vec4	eyeZ;
	// user clip plane in object space (zero when there is none)
	vec4	clipPlane;
#ifdef TEMPORAL
	// the temporal path's block (vkTemporalPush_t in vk_local.h)
	layout(offset = 128) mat4	prevMvp;	// last frame's projection (unjittered) * modelview
	vec4	mvRow0;			// rows 0 and 1 of the modelview (row 2 is eyeZ)
	vec4	mvRow1;
	vec4	jitter;			// xy: the projection's jitter in NDC, z: the surface class, w: the texture LOD bias
#endif
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_st0;
layout(location = 3) in vec2 in_st1;
#ifdef TEMPORAL
// (0, 0, 0) with a binding stride of 0 when the draw has no normals
layout(location = 4) in vec3 in_normal;
#endif

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_st0;
layout(location = 2) out vec2 v_st1;
// x: fog distance |z_eye|, y: distance to the clip plane (generic.frag
// discards with it when gl_ClipDistance isn't available)
layout(location = 3) out vec2 v_fogClip;
// object space position, for the dynamic lights (only pipelines with them
// read it; the others link it away)
layout(location = 4) out vec3 v_position;
// bindless textures: the draw's units' slots, unit 0 | unit 1 << 12 in
// firstInstance (vk_draw); unused without them
layout(location = 11) flat out uvec2 v_tex;
#ifdef TEMPORAL
// the temporal path's outputs (generic.frag): clip positions of this frame
// without the jitter and of the last frame, the view space normal and
// position, and the surface class
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

#ifdef TEMPORAL
// the temporal path: only the pipelines that write the normals buffer
// (vk_pipeline.c) compute what it needs, the motion vectors' only those
// that draw what moves (generic.frag)
layout(constant_id = 11) const bool TEMPORAL_NORMALS = true;
layout(constant_id = 12) const bool TEMPORAL_MOTION = true;
#endif

// multi-pass shaders use GLS_DEPTHFUNC_EQUAL: every pipeline must produce
// bit-identical depth for the same vertex
invariant gl_Position;

void main() {
	v_tex = uvec2( uint( gl_InstanceIndex ) & 0xFFFu, uint( gl_InstanceIndex ) >> 12 );

	// the operations of Mesa's fixed-function vertex program (a multiply,
	// then fused multiply-adds, column by column), so clip coordinates are
	// bit-identical to GL1's on Mesa drivers
	vec4 pos = pc.mvp[0] * in_position.x;
	pos = fma( pc.mvp[1], vec4( in_position.y ), pos );
	pos = fma( pc.mvp[2], vec4( in_position.z ), pos );
	gl_Position = pos + pc.mvp[3];

	// fog coordinate of Mesa's fixed-function vertex program in its default
	// mode (eye plane, absolute): the z of modelview * position, computed
	// with the same operations as the position
	float eyeZ = pc.eyeZ.x * in_position.x;
	eyeZ = fma( pc.eyeZ.y, in_position.y, eyeZ );
	eyeZ = fma( pc.eyeZ.z, in_position.z, eyeZ );
	v_fogClip.x = abs( eyeZ + pc.eyeZ.w );

	v_fogClip.y = dot( pc.clipPlane, vec4( in_position, 1.0 ) );
#ifdef CLIP_DISTANCE
	gl_ClipDistance[0] = v_fogClip.y;
#endif

	v_color = in_color;
	v_st0 = in_st0;
	v_st1 = in_st1;
	v_position = in_position;

#ifdef TEMPORAL
	v_lodBias = pc.jitter.w;
	if ( !TEMPORAL_NORMALS ) {
		return;
	}
	if ( TEMPORAL_MOTION ) {
		// the projection's jitter moves x and y by jitter * w
		v_currClip = gl_Position - vec4( pc.jitter.xy * gl_Position.w, 0.0, 0.0 );
		v_prevClip = pc.prevMvp * vec4( in_position, 1.0 );
	}
	v_viewPos = vec3( dot( pc.mvRow0, vec4( in_position, 1.0 ) ), dot( pc.mvRow1, vec4( in_position, 1.0 ) ),
		dot( pc.eyeZ, vec4( in_position, 1.0 ) ) );
	v_viewNormal = vec3( dot( pc.mvRow0.xyz, in_normal ), dot( pc.mvRow1.xyz, in_normal ), dot( pc.eyeZ.xyz, in_normal ) );
	v_class = pc.jitter.z;
#endif
}
