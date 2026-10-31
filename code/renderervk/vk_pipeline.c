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

// vk_pipeline.c -- graphics pipelines for the fixed-function state
//
// vk.c packs the state that has to be baked into a pipeline into a
// vkPipelineKey_t (VK_PIPE_* fields); pipelines are kept in a hash table for
// the lifetime of the device, created by the end of a registration for the
// draws its shaders will make (vk_prebuild_pipeline, tr_pipelines.c) or on
// first use. Viewport, scissor, depth bias, line width and the stencil
// reference and masks are dynamic.
//
// The VkPipelineCache is saved to VK_PIPELINE_CACHE_FILE in fs_homepath at
// shutdown and given back to the driver at startup when it was made by the
// same device and driver, so pipelines don't have to be compiled again. The
// keys of the pipelines created after a load are saved to
// VK_PIPELINE_KEYS_FILE and built at the end of the next runs' loads (see
// PIPELINES AT LOAD).
//
// Every pipeline uses the same shaders (shaders/generic.*); the fragment
// stage is specialized for the texture envs, the alpha test, fog, the
// dynamic lights added to a lightmap and, when the device can't clip with
// gl_ClipDistance, the clip plane. Pipelines with
// a clip plane use the vertex shader variant that writes gl_ClipDistance.

#include "vk_local.h"

// what created a pipeline
typedef enum {
	VK_ORIGIN_DRAW,			// a draw that needed it (VK_GetPipeline)
	VK_ORIGIN_LOAD,			// the end of a registration (tr_pipelines.c)
	VK_ORIGIN_LATE,			// a shader registered after it
	VK_ORIGIN_RECORDED		// VK_PIPELINE_KEYS_FILE
} vkPipelineOrigin_t;

typedef struct {
	vkPipelineKey_t	key;
	VkPipeline		pipeline;
	byte			origin;		// vkPipelineOrigin_t
	byte			used;		// a draw has used it
} vkPipelineEntry_t;

#define VK_MAX_PREBUILT		1024	// pipelines created by a registration before a draw needs them

#define VK_PIPELINE_CACHE_FILE	"vkpipelines.cache"

static size_t				vk_pipelineCacheLoaded;	// size of the data read at startup

static vkPipelineEntry_t	*vk_pipelines;
static int					vk_pipelineTableSize;	// power of two
static int					vk_numPipelines;
static qboolean				vk_prebuildLate;		// the registration's pipelines are built (vk_end_prebuild)
static int					vk_prebuiltNow;			// by this registration, or after it

/*
====================================================================

CREATION

====================================================================
*/

static VkBlendFactor VK_SrcBlendFactor( int bits ) {
	switch ( bits ) {
	case GLS_SRCBLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
	case GLS_SRCBLEND_ONE: return VK_BLEND_FACTOR_ONE;
	case GLS_SRCBLEND_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
	case GLS_SRCBLEND_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case GLS_SRCBLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
	case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	// no destination alpha (see VK_CreatePipeline): it reads as 1
	case GLS_SRCBLEND_DST_ALPHA: return VK_BLEND_FACTOR_ONE;
	case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ZERO;
	case GLS_SRCBLEND_ALPHA_SATURATE: return VK_BLEND_FACTOR_ZERO;	// min( As, 1 - Ad )
	default: return VK_BLEND_FACTOR_ONE;
	}
}

static VkBlendFactor VK_DstBlendFactor( int bits ) {
	switch ( bits ) {
	case GLS_DSTBLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
	case GLS_DSTBLEND_ONE: return VK_BLEND_FACTOR_ONE;
	case GLS_DSTBLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
	case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case GLS_DSTBLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
	case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case GLS_DSTBLEND_DST_ALPHA: return VK_BLEND_FACTOR_ONE;
	case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ZERO;
	default: return VK_BLEND_FACTOR_ONE;
	}
}

static VkStencilOp VK_StencilOp( int op ) {
	switch ( op ) {
	case STENCILOP_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
	case STENCILOP_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
	default: return VK_STENCIL_OP_KEEP;
	}
}

#define VK_FIELD( bits, shift, mask ) ( (int)( ( ( bits ) >> ( shift ) ) & ( mask ) ) )

static VkPipeline VK_CreatePipeline( const vkPipelineKey_t *key ) {
	// fragment shader specialization constants, see shaders/generic.frag
	struct {
		int32_t		texEnv0;
		int32_t		texEnv1;
		VkBool32	tex0Rgb;
		VkBool32	tex1Rgb;
		int32_t		alphaFunc;
		float		alphaRef;
		VkBool32	fog;
		VkBool32	clipDiscard;
		VkBool32	alphaOne;
		int32_t		dlightUnit;
		VkBool32	dlightBacks;
		VkBool32	temporalNormals;	// the temporal path's shaders only
		VkBool32	temporalMotion;
		int32_t		temporalReactive;
	} specData;
	VkSpecializationMapEntry specEntries[14];
	VkSpecializationInfo specInfo;
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription bindings[5];
	VkVertexInputAttributeDescription attributes[5];
	VkPipelineVertexInputStateCreateInfo vertexInput;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly;
	VkPipelineViewportStateCreateInfo viewportState;
	VkPipelineViewportDepthClipControlCreateInfoEXT depthClipControl;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineRasterizationLineStateCreateInfoEXT lineState;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depthStencil;
	VkPipelineColorBlendAttachmentState blendAttachments[3];
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState dynamicStates[8];
	VkPipelineDynamicStateCreateInfo dynamicState;
	VkGraphicsPipelineCreateInfo createInfo;
	VkPipeline pipeline;
	uint64_t bits, start;
	int colorMask, srcBlend, dstBlend, i;
	qboolean lines, temporal;

	bits = key->bits;
	lines = ( bits & VK_PIPE_LINES ) != 0;
	temporal = ( bits & VK_PIPE_TEMPORAL ) != 0;

	// shaders
	specData.texEnv0 = VK_FIELD( bits, VK_PIPE_TEXENV0_SHIFT, 7 );
	specData.texEnv1 = VK_FIELD( bits, VK_PIPE_TEXENV1_SHIFT, 7 );
	specData.tex0Rgb = ( bits & VK_PIPE_TEX0_RGB ) ? VK_TRUE : VK_FALSE;
	specData.tex1Rgb = ( bits & VK_PIPE_TEX1_RGB ) ? VK_TRUE : VK_FALSE;
	specData.alphaFunc = VK_FIELD( bits, VK_PIPE_ATEST_SHIFT, 3 );
	Com_Memcpy( &specData.alphaRef, &key->alphaRef, sizeof( float ) );
	specData.fog = ( bits & VK_PIPE_FOG ) ? VK_TRUE : VK_FALSE;
	specData.clipDiscard = ( ( bits & VK_PIPE_CLIPPLANE ) && !vk.enabledFeatures.shaderClipDistance ) ? VK_TRUE : VK_FALSE;
	srcBlend = VK_FIELD( bits, VK_PIPE_SRCBLEND_SHIFT, 15 );
	dstBlend = VK_FIELD( bits, VK_PIPE_DSTBLEND_SHIFT, 15 ) << 4;
	specData.alphaOne = ( srcBlend || dstBlend ) ? VK_FALSE : VK_TRUE;
	specData.dlightUnit = VK_FIELD( bits, VK_PIPE_DLIGHT_SHIFT, 3 );
	specData.dlightBacks = ( bits & VK_PIPE_DLIGHT_BACKS ) ? VK_TRUE : VK_FALSE;
	// the temporal path: the pipelines whose write masks drop the normals
	// buffer skip its work, those that draw nothing that moves the motion
	// vectors' (all their shaders read these)
	specData.temporalNormals = VK_TemporalWritesBuffers( bits ) ? VK_TRUE : VK_FALSE;
	specData.temporalMotion = specData.temporalNormals && ( bits & VK_PIPE_MOTION ) ? VK_TRUE : VK_FALSE;
	// the reactive share of a blended effect in the scene's alpha
	// (VK_PIPE_REACTIVE): how much it changes what is behind it. Its
	// brightest channel times its alpha where it adds with its alpha (4:
	// drawn premultiplied, ONE instead of SRC_ALPHA below, as an RGB
	// texture's corona has the same alpha all over its quad), the surface's
	// alpha where the blend reads it otherwise (1), its brightest channel
	// where it adds (2), one minus its darkest where it multiplies (3)
	specData.temporalReactive = 0;
	if ( bits & VK_PIPE_REACTIVE ) {
		VkBlendFactor sf = VK_SrcBlendFactor( srcBlend ), df = VK_DstBlendFactor( dstBlend );

		if ( sf == VK_BLEND_FACTOR_SRC_ALPHA && df == VK_BLEND_FACTOR_ONE ) {
			specData.temporalReactive = 4;
		} else if ( sf == VK_BLEND_FACTOR_SRC_ALPHA || sf == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
			|| df == VK_BLEND_FACTOR_SRC_ALPHA || df == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ) {
			specData.temporalReactive = 1;
		} else if ( ( sf == VK_BLEND_FACTOR_DST_COLOR || sf == VK_BLEND_FACTOR_ZERO ) && ( df == VK_BLEND_FACTOR_ZERO || df == VK_BLEND_FACTOR_SRC_COLOR ) ) {
			specData.temporalReactive = 3;
		} else {
			specData.temporalReactive = 2;
		}
	}

	for ( i = 0; i < (int)ARRAY_LEN( specEntries ); i++ ) {
		specEntries[i].constantID = i;
		specEntries[i].offset = i * 4;
		specEntries[i].size = 4;
	}
	// the shaders without the temporal path have no constants 11 to 13
	specInfo.mapEntryCount = temporal ? 14 : 11;
	specInfo.pMapEntries = specEntries;
	specInfo.dataSize = sizeof( specData );
	specInfo.pData = &specData;

	Com_Memset( stages, 0, sizeof( stages ) );
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	if ( temporal ) {
		// the temporal path's variants (vk_temporal.c)
		if ( bits & VK_PIPE_SKINNED ) {
			stages[0].module = ( ( bits & VK_PIPE_CLIPPLANE ) && vkt.skinVertexShaderClip ) ? vkt.skinVertexShaderClip : vkt.skinVertexShader;
		} else if ( bits & VK_PIPE_EFFECTS ) {
			stages[0].module = ( ( bits & VK_PIPE_CLIPPLANE ) && vkt.effectsVertexShaderClip ) ? vkt.effectsVertexShaderClip : vkt.effectsVertexShader;
		} else {
			stages[0].module = ( ( bits & VK_PIPE_CLIPPLANE ) && vkt.vertexShaderClip ) ? vkt.vertexShaderClip : vkt.vertexShader;
		}
	} else if ( bits & VK_PIPE_SKINNED ) {
		stages[0].module = ( ( bits & VK_PIPE_CLIPPLANE ) && vk.skinVertexShaderClip ) ? vk.skinVertexShaderClip : vk.skinVertexShader;
	} else if ( bits & VK_PIPE_EFFECTS ) {
		stages[0].module = ( ( bits & VK_PIPE_CLIPPLANE ) && vk.effectsVertexShaderClip ) ? vk.effectsVertexShaderClip : vk.effectsVertexShader;
	} else {
		stages[0].module = ( ( bits & VK_PIPE_CLIPPLANE ) && vk.vertexShaderClip ) ? vk.vertexShaderClip : vk.vertexShader;
	}
	stages[0].pName = "main";
	if ( temporal ) {
		stages[0].pSpecializationInfo = &specInfo;
	}
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = temporal ? vkt.fragmentShader : vk.fragmentShader;
	stages[1].pName = "main";
	stages[1].pSpecializationInfo = &specInfo;

	// vertex streams, each in its own binding (see VK_UploadGeometry);
	// constant colors and missing texture coordinates use a stride of 0
	// (but see vk.strideZero). Effects draws read the colors per vertex
	// (or the constant in their record), both texture coordinate streams of
	// the static geometry and its normals (VK_BindStaticGeometry): no
	// stride of 0
	for ( i = 0; i < 5; i++ ) {
		bindings[i].binding = i;
		bindings[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		attributes[i].location = i;
		attributes[i].binding = i;
		attributes[i].offset = 0;
	}
	bindings[0].stride = sizeof( float ) * 3;
	attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
	if ( bits & ( VK_PIPE_COLOR_ARRAY | VK_PIPE_EFFECTS ) ) {
		bindings[1].stride = 4;
		attributes[1].format = VK_FORMAT_R8G8B8A8_UNORM;
	} else {
		bindings[1].stride = 0;
		attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	}
	bindings[2].stride = specData.texEnv0 || ( bits & VK_PIPE_EFFECTS ) ? sizeof( float ) * 2 : 0;
	attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
	bindings[3].stride = specData.texEnv1 || ( bits & VK_PIPE_EFFECTS ) ? sizeof( float ) * 2 : 0;
	attributes[3].format = VK_FORMAT_R32G32_SFLOAT;
	bindings[4].stride = sizeof( float ) * 3;
	attributes[4].format = VK_FORMAT_R32G32B32_SFLOAT;
	if ( temporal && !( bits & ( VK_PIPE_EFFECTS | VK_PIPE_NORMALS ) ) ) {
		// the temporal path's generic draws without normals read (0, 0, 0)
		bindings[4].stride = 0;
	}
	if ( !vk.strideZero ) {
		// the portability subset allows no stride of 0 (nor an attribute
		// beyond the stride without vertexAttributeAccessBeyondStride): those
		// bindings are read per instance with the size of their attribute as
		// the stride. Every draw that has such a binding has one instance,
		// the first, so they read the same bytes (effects draws, whose
		// firstInstance is their record, have none)
		for ( i = 1; i < 5; i++ ) {
			if ( !bindings[i].stride ) {
				bindings[i].stride = i == 1 ? sizeof( float ) * 4 : i == 4 ? sizeof( float ) * 3 : sizeof( float ) * 2;
				bindings[i].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
			}
		}
	}

	Com_Memset( &vertexInput, 0, sizeof( vertexInput ) );
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	if ( !( bits & VK_PIPE_SKINNED ) ) {
		// skinned draws read their vertexes from storage buffers; only
		// effects draws and the temporal path's have normals
		vertexInput.vertexBindingDescriptionCount = ( bits & VK_PIPE_EFFECTS ) || temporal ? 5 : 4;
		vertexInput.pVertexBindingDescriptions = bindings;
		vertexInput.vertexAttributeDescriptionCount = vertexInput.vertexBindingDescriptionCount;
		vertexInput.pVertexAttributeDescriptions = attributes;
	}

	Com_Memset( &inputAssembly, 0, sizeof( inputAssembly ) );
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	Com_Memset( &viewportState, 0, sizeof( viewportState ) );
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;
	if ( vk.depthClipControl ) {
		Com_Memset( &depthClipControl, 0, sizeof( depthClipControl ) );
		depthClipControl.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT;
		depthClipControl.negativeOneToOne = VK_TRUE;
		viewportState.pNext = &depthClipControl;
	}

	Com_Memset( &rasterization, 0, sizeof( rasterization ) );
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = ( ( bits & VK_PIPE_POLYMODE_LINE ) && !lines ) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	switch ( VK_FIELD( bits, VK_PIPE_CULL_SHIFT, 3 ) ) {
	case CULL_FRONT:
		rasterization.cullMode = VK_CULL_MODE_FRONT_BIT;
		break;
	case CULL_BACK:
		rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
		break;
	default:
		rasterization.cullMode = VK_CULL_MODE_NONE;
		break;
	}
	// with the y flip of the viewport, counter-clockwise matches GL
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.depthBiasEnable = ( bits & VK_PIPE_DEPTHBIAS ) ? VK_TRUE : VK_FALSE;
	rasterization.depthClampEnable = ( bits & VK_PIPE_DEPTHCLAMP ) ? VK_TRUE : VK_FALSE;
	rasterization.lineWidth = 1.0f;
	if ( vk.bresenhamLines && ( lines || rasterization.polygonMode == VK_POLYGON_MODE_LINE ) ) {
		// GL's line rasterization, with stippling
		Com_Memset( &lineState, 0, sizeof( lineState ) );
		lineState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT;
		lineState.lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT;
		lineState.stippledLineEnable = ( bits & VK_PIPE_STIPPLE ) ? VK_TRUE : VK_FALSE;
		lineState.lineStippleFactor = 1;
		lineState.lineStipplePattern = 0xFFFF;
		rasterization.pNext = &lineState;
	}

	Com_Memset( &multisample, 0, sizeof( multisample ) );
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = vk.samples;

	Com_Memset( &depthStencil, 0, sizeof( depthStencil ) );
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = ( bits & VK_PIPE_DEPTHTEST ) ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = ( bits & VK_PIPE_DEPTHWRITE ) ? VK_TRUE : VK_FALSE;
	switch ( VK_FIELD( bits, VK_PIPE_DEPTHFUNC_SHIFT, 3 ) ) {
	case VK_DEPTHFUNC_EQUAL:
		depthStencil.depthCompareOp = VK_COMPARE_OP_EQUAL;
		break;
	case VK_DEPTHFUNC_ALWAYS:
		depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
		break;
	default:
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		break;
	}
	if ( bits & VK_PIPE_STENCIL ) {
		depthStencil.stencilTestEnable = VK_TRUE;
		depthStencil.front.compareOp = VK_FIELD( bits, VK_PIPE_STENCILFUNC_SHIFT, 1 ) == STENCILFUNC_NOTEQUAL
			? VK_COMPARE_OP_NOT_EQUAL : VK_COMPARE_OP_ALWAYS;
		depthStencil.front.failOp = VK_StencilOp( VK_FIELD( bits, VK_PIPE_STENCILOP_SHIFT, 3 ) );
		depthStencil.front.depthFailOp = VK_StencilOp( VK_FIELD( bits, VK_PIPE_STENCILOP_SHIFT + 2, 3 ) );
		depthStencil.front.passOp = VK_StencilOp( VK_FIELD( bits, VK_PIPE_STENCILOP_SHIFT + 4, 3 ) );
		depthStencil.back = depthStencil.front;
	}
	depthStencil.minDepthBounds = 0.0f;
	depthStencil.maxDepthBounds = 1.0f;

	// the framebuffer has no destination alpha, like the GL1 visual: blend
	// factors read it as 1 and nothing else reads it. Pipelines that write
	// all of RGB without blending write alpha too, as 1 (alphaOne): a write
	// mask without alpha is a partial write, which costs DCC-compressed
	// targets (Radeon 780M: 1 to 2 % of the GPU time at 3840x2160, 4 % in
	// views that clear the color buffer first; 6 % with 4 samples in the
	// m6l1a forest). With blending they write it too only with
	// multisampling, unchanged: without multisampling RADV then rounds 1 in
	// about 10^5 blended pixels one step lower, unlike GL1, and it saves
	// nothing there (blending reads the target anyway).
	colorMask = VK_FIELD( bits, VK_PIPE_COLORMASK_SHIFT, 7 );

	Com_Memset( blendAttachments, 0, sizeof( blendAttachments ) );
	if ( srcBlend || dstBlend ) {
		blendAttachments[0].blendEnable = VK_TRUE;
		blendAttachments[0].srcColorBlendFactor = VK_SrcBlendFactor( srcBlend );
		blendAttachments[0].dstColorBlendFactor = VK_DstBlendFactor( dstBlend );
		blendAttachments[0].colorBlendOp = VK_BLEND_OP_ADD;
		blendAttachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blendAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
	}
	blendAttachments[0].colorWriteMask = ( ( colorMask & 1 ) ? VK_COLOR_COMPONENT_R_BIT : 0 )
		| ( ( colorMask & 2 ) ? VK_COLOR_COMPONENT_G_BIT : 0 )
		| ( ( colorMask & 4 ) ? VK_COLOR_COMPONENT_B_BIT : 0 )
		| ( colorMask == 7 && ( vk.samples != VK_SAMPLE_COUNT_1_BIT || !( srcBlend || dstBlend ) ) ? VK_COLOR_COMPONENT_A_BIT : 0 );

	Com_Memset( &blend, 0, sizeof( blend ) );
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = blendAttachments;
	if ( temporal ) {
		// the motion vectors and the normals: written by the surfaces that
		// write depth without blending, those that hide what is behind
		// them; the others leave those of what they are drawn over
		if ( specData.temporalMotion ) {
			blendAttachments[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
		}
		if ( specData.temporalNormals ) {
			blendAttachments[2].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
				| VK_COLOR_COMPONENT_A_BIT;
		}
		// the scene's alpha is the reactive share, 0 where nothing reactive
		// was drawn (the draws without blending write 0, generic.frag): the
		// effects' shares add up as coverage does, a + d ( 1 - a ), at most
		// 1. (MAX, against the color's ADD, cost 0.027 ms more GPU time a
		// frame on an RTX 4070 Ti at 2560x1440 with the sun's lens flare in
		// view; this, nothing measurable)
		if ( specData.temporalReactive ) {
			if ( specData.temporalReactive == 4 ) {
				// the shader multiplies by its alpha
				blendAttachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
			}
			blendAttachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			blendAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			blendAttachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
			blendAttachments[0].colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
		}
		blend.attachmentCount = 3;
	}

	dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
	dynamicStates[2] = VK_DYNAMIC_STATE_LINE_WIDTH;
	dynamicStates[3] = VK_DYNAMIC_STATE_DEPTH_BIAS;
	dynamicStates[4] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
	dynamicStates[5] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
	dynamicStates[6] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;

	Com_Memset( &dynamicState, 0, sizeof( dynamicState ) );
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 7;
	if ( bits & VK_PIPE_STIPPLE ) {
		dynamicStates[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_LINE_STIPPLE_EXT;
	}
	dynamicState.pDynamicStates = dynamicStates;

	Com_Memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	createInfo.stageCount = ARRAY_LEN( stages );
	createInfo.pStages = stages;
	createInfo.pVertexInputState = &vertexInput;
	createInfo.pInputAssemblyState = &inputAssembly;
	createInfo.pViewportState = &viewportState;
	createInfo.pRasterizationState = &rasterization;
	createInfo.pMultisampleState = &multisample;
	createInfo.pDepthStencilState = &depthStencil;
	createInfo.pColorBlendState = &blend;
	createInfo.pDynamicState = &dynamicState;
	createInfo.layout = temporal ? vkt.pipelineLayout : vk.pipelineLayout;
	createInfo.renderPass = temporal ? vkt.renderPass : vk.renderPass;
	createInfo.subpass = 0;

	start = VK_Microseconds();
	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, vk.pipelineCache, 1, &createInfo, NULL, &pipeline ) );
	vk.stats.pipelineUs += VK_Microseconds() - start;
	vk.stats.pipelines++;

	return pipeline;
}

/*
====================================================================

CACHE

====================================================================
*/

static unsigned int VK_HashKey( const vkPipelineKey_t *key ) {
	uint64_t h;

	h = key->bits ^ ( (uint64_t)key->alphaRef << 29 );
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ull;
	h ^= h >> 33;
	return (unsigned int)h;
}

static vkPipelineEntry_t *VK_InsertPipeline( vkPipelineEntry_t *table, int size, const vkPipelineEntry_t *entry ) {
	int i;

	for ( i = VK_HashKey( &entry->key ) & ( size - 1 ); table[i].pipeline; i = ( i + 1 ) & ( size - 1 ) ) {
	}
	table[i] = *entry;
	return &table[i];
}

static vkPipelineEntry_t *VK_FindPipeline( const vkPipelineKey_t *key ) {
	int i;

	for ( i = VK_HashKey( key ) & ( vk_pipelineTableSize - 1 ); vk_pipelines[i].pipeline; i = ( i + 1 ) & ( vk_pipelineTableSize - 1 ) ) {
		if ( vk_pipelines[i].key.bits == key->bits && vk_pipelines[i].key.alphaRef == key->alphaRef ) {
			return &vk_pipelines[i];
		}
	}
	return NULL;
}

// a table of size entries (a power of two, at least twice the entries kept)
// with the entries of the current one that keep() accepts
static void VK_RebuildTable( int size, qboolean ( *keep )( const vkPipelineEntry_t *entry ) ) {
	vkPipelineEntry_t *table;
	int i, n = 0;

	table = VK_Alloc( sizeof( *table ) * size );
	Com_Memset( table, 0, sizeof( *table ) * size );
	for ( i = 0; i < vk_pipelineTableSize; i++ ) {
		if ( vk_pipelines[i].pipeline ) {
			if ( !keep || keep( &vk_pipelines[i] ) ) {
				VK_InsertPipeline( table, size, &vk_pipelines[i] );
				n++;
			} else {
				qvkDestroyPipeline( vk.device, vk_pipelines[i].pipeline, NULL );
			}
		}
	}
	VK_Free( vk_pipelines );
	vk_pipelines = table;
	vk_pipelineTableSize = size;
	vk_numPipelines = n;
}

static vkPipelineEntry_t *VK_AddPipeline( const vkPipelineKey_t *key, vkPipelineOrigin_t origin ) {
	vkPipelineEntry_t entry;

	// keep the table at most half full
	if ( ( vk_numPipelines + 1 ) * 2 > vk_pipelineTableSize ) {
		VK_RebuildTable( vk_pipelineTableSize * 2, NULL );
	}

	Com_Memset( &entry, 0, sizeof( entry ) );
	entry.key = *key;
	entry.pipeline = VK_CreatePipeline( key );
	entry.origin = origin;
	vk_numPipelines++;
	return VK_InsertPipeline( vk_pipelines, vk_pipelineTableSize, &entry );
}

VkPipeline VK_GetPipeline( const vkPipelineKey_t *key ) {
	vkPipelineEntry_t *entry = VK_FindPipeline( key );

	if ( !entry ) {
		entry = VK_AddPipeline( key, VK_ORIGIN_DRAW );
	}
	entry->used = qtrue;
	return entry->pipeline;
}

/*
===============
VK_PrebuildKey

Creates the pipeline of a key that a draw is expected to make (see
tr_pipelines.c) unless it exists; returns whether it was created. Bounded
per registration (VK_MAX_PREBUILT): what comes after is left to the draws
===============
*/
static qboolean VK_PrebuildKey( const vkPipelineKey_t *key, vkPipelineOrigin_t origin ) {
	uint64_t start;

	if ( VK_FindPipeline( key ) ) {
		return qfalse;
	}
	if ( vk_prebuiltNow >= VK_MAX_PREBUILT ) {
		if ( vk_prebuiltNow++ == VK_MAX_PREBUILT ) {
			ri.Printf( PRINT_WARNING, "Vulkan: more than %d pipelines to create at load, the others are created at their first draw\n",
				VK_MAX_PREBUILT );
		}
		return qfalse;
	}
	vk_prebuiltNow++;
	start = vk.stats.pipelineUs;
	VK_AddPipeline( key, origin );
	vk.stats.prebuiltPipelines++;
	vk.stats.prebuiltUs += vk.stats.pipelineUs - start;
	return qtrue;
}

qboolean VK_PrebuildPipeline( const vkPipelineKey_t *key ) {
	return VK_PrebuildKey( key, vk_prebuildLate ? VK_ORIGIN_LATE : VK_ORIGIN_LOAD );
}

/*
===============
VK_PipelineCounts

The pipelines that exist: all of them, those created before a draw needed
them (at load, for a shader registered later, recorded in earlier runs) and
those of them that a draw has used
===============
*/
void VK_PipelineCounts( int *total, int *prebuilt, int *prebuiltUsed ) {
	int i;

	*total = vk_numPipelines;
	*prebuilt = *prebuiltUsed = 0;
	for ( i = 0; i < vk_pipelineTableSize; i++ ) {
		if ( vk_pipelines[i].pipeline && vk_pipelines[i].origin != VK_ORIGIN_DRAW ) {
			( *prebuilt )++;
			if ( vk_pipelines[i].used ) {
				( *prebuiltUsed )++;
			}
		}
	}
}

int VK_NumPipelines( void ) {
	return vk_numPipelines;
}

/*
====================================================================

PIPELINES AT LOAD

tr_pipelines.c builds the pipelines of a registration's shaders at its end;
what it can't foresee (shaders registered later, the immediate mode draws
of the UI after whatever state came before) is created at the first draw
or when the shader is registered. Those keys are written to
VK_PIPELINE_KEYS_FILE at shutdown and built at the end of every
registration of the next runs: after a driver update, when
VK_PIPELINE_CACHE_FILE no longer applies, they are compiled at load too.
The file holds keys only, so it outlives the driver; a new layout of the
VK_PIPE_* fields needs a new VK_PIPELINE_KEYS_VERSION

====================================================================
*/

#define VK_PIPELINE_KEYS_FILE		"vkpipelines.keys"
#define VK_PIPELINE_KEYS_MAGIC		0x4b504b56		// "VKPK"
#define VK_PIPELINE_KEYS_VERSION	1
#define VK_MAX_RECORDED_KEYS		256

typedef struct {
	uint64_t	bits;
	uint32_t	alphaRef;
	uint32_t	unused;
} vkRecordedKey_t;

static vkRecordedKey_t	vk_recordedKeys[VK_MAX_RECORDED_KEYS];
static int				vk_numRecordedKeys;		// read at startup

// what VK_BuildPipelineKey can make on this device
static qboolean VK_KeyValid( uint64_t bits ) {
	if ( ( bits >> 46 ) & ~( ( VK_PIPE_DEPTHCLAMP | VK_PIPE_TEMPORAL | VK_PIPE_NORMALS | VK_PIPE_MOTION | VK_PIPE_REACTIVE ) >> 46 ) ) {
		return qfalse;
	}
	if ( ( bits & ( VK_PIPE_TEMPORAL | VK_PIPE_NORMALS | VK_PIPE_MOTION | VK_PIPE_REACTIVE ) ) && !vkt.enabled ) {
		return qfalse;
	}
	// the reactive share only for the temporal path's blended draws that
	// write no normals
	if ( ( bits & VK_PIPE_REACTIVE ) && ( !( bits & VK_PIPE_TEMPORAL ) || !( bits & VK_PIPE_BLEND_BITS ) || VK_TemporalWritesBuffers( bits ) ) ) {
		return qfalse;
	}
	// normals and motion only for the temporal path's pipelines that write them
	if ( ( bits & ( VK_PIPE_NORMALS | VK_PIPE_MOTION ) ) && ( !( bits & VK_PIPE_TEMPORAL ) || !VK_TemporalWritesBuffers( bits ) ) ) {
		return qfalse;
	}
	if ( ( bits & VK_PIPE_DEPTHCLAMP ) && !vk.enabledFeatures.depthClamp ) {
		return qfalse;
	}
	if ( ( bits & VK_PIPE_STIPPLE ) && !vk.lineStipple ) {
		return qfalse;
	}
	if ( ( bits & VK_PIPE_POLYMODE_LINE ) && !vk.enabledFeatures.fillModeNonSolid ) {
		return qfalse;
	}
	return qtrue;
}

static void VK_ReadRecordedKeys( void ) {
	uint32_t header[4];
	void *data;
	int len;

	vk_numRecordedKeys = 0;
	len = ri.FS_ReadFileEx( VK_PIPELINE_KEYS_FILE, &data, qtrue );
	if ( len <= 0 || !data ) {
		return;
	}
	// magic, version, count, 0, then the keys
	Com_Memset( header, 0, sizeof( header ) );
	Com_Memcpy( header, data, Q_min( len, (int)sizeof( header ) ) );
	if ( header[0] != VK_PIPELINE_KEYS_MAGIC || header[1] != VK_PIPELINE_KEYS_VERSION || header[2] > VK_MAX_RECORDED_KEYS
		|| len != (int)( sizeof( header ) + header[2] * sizeof( vkRecordedKey_t ) ) ) {
		ri.Printf( PRINT_ALL, "Vulkan: %s is from another version, ignored\n", VK_PIPELINE_KEYS_FILE );
	} else {
		vk_numRecordedKeys = header[2];
		Com_Memcpy( vk_recordedKeys, (const byte *)data + sizeof( header ), vk_numRecordedKeys * sizeof( vkRecordedKey_t ) );
	}
	ri.FS_FreeFile( data );
}

// what the file will hold: the keys that came after the load this run
// (created by a draw, or for a shader registered later), the recorded ones
// that a draw used, then the others; returns how many, *added: of the first
static int VK_CollectRecordedKeys( vkRecordedKey_t *out, int *added ) {
	int pass, i, j, n = 0;

	*added = 0;
	for ( pass = 0; pass < 3; pass++ ) {
		for ( i = 0; i < vk_pipelineTableSize && n < VK_MAX_RECORDED_KEYS; i++ ) {
			const vkPipelineEntry_t *e = &vk_pipelines[i];

			if ( !e->pipeline || ( pass ? e->origin != VK_ORIGIN_RECORDED || !e->used != ( pass == 2 )
				: e->origin != VK_ORIGIN_DRAW && e->origin != VK_ORIGIN_LATE ) ) {
				continue;
			}
			if ( !pass ) {
				for ( j = 0; j < vk_numRecordedKeys; j++ ) {
					if ( vk_recordedKeys[j].bits == e->key.bits && vk_recordedKeys[j].alphaRef == e->key.alphaRef ) {
						break;
					}
				}
				if ( j < vk_numRecordedKeys ) {
					continue;
				}
				( *added )++;
			}
			Com_Memset( &out[n], 0, sizeof( out[n] ) );
			out[n].bits = e->key.bits;
			out[n].alphaRef = e->key.alphaRef;
			n++;
		}
	}
	// the other recorded keys: built otherwise (at load, by a draw), or not
	// (not valid on this device, more than VK_MAX_PREBUILT, released)
	for ( i = 0; i < vk_numRecordedKeys && n < VK_MAX_RECORDED_KEYS; i++ ) {
		vkPipelineKey_t key;
		const vkPipelineEntry_t *e;

		key.bits = vk_recordedKeys[i].bits;
		key.alphaRef = vk_recordedKeys[i].alphaRef;
		e = VK_FindPipeline( &key );
		if ( !e || e->origin != VK_ORIGIN_RECORDED ) {
			out[n++] = vk_recordedKeys[i];
		}
	}
	return n;
}

static void VK_WriteRecordedKeys( void ) {
	vkRecordedKey_t *keys;
	uint32_t *data;
	int n, added;

	keys = VK_Alloc( sizeof( *keys ) * VK_MAX_RECORDED_KEYS );
	n = VK_CollectRecordedKeys( keys, &added );
	if ( added ) {
		data = VK_Alloc( 16 + n * sizeof( *keys ) );
		data[0] = VK_PIPELINE_KEYS_MAGIC;
		data[1] = VK_PIPELINE_KEYS_VERSION;
		data[2] = n;
		data[3] = 0;
		Com_Memcpy( data + 4, keys, n * sizeof( *keys ) );
		ri.FS_WriteFile( VK_PIPELINE_KEYS_FILE, data, 16 + n * sizeof( *keys ) );
		VK_Free( data );
	}
	VK_Free( keys );
}

static qboolean VK_KeepUsed( const vkPipelineEntry_t *entry ) {
	return entry->origin == VK_ORIGIN_DRAW || entry->used;
}

/*
===============
vk_begin_prebuild

A registration starts (RE_BeginRegistration): the pipelines built before a
draw needed them that no draw has used are destroyed (no command buffer
refers to them), so what is kept is bounded by the draws and the last
registration's
===============
*/
void vk_begin_prebuild( void ) {
	int i, unused = 0;

	vk_prebuildLate = qfalse;
	vk_prebuiltNow = 0;
	if ( !vk.device || !vk_pipelines ) {
		return;
	}
	for ( i = 0; i < vk_pipelineTableSize; i++ ) {
		if ( vk_pipelines[i].pipeline && !VK_KeepUsed( &vk_pipelines[i] ) ) {
			unused++;
		}
	}
	if ( unused ) {
		VK_RebuildTable( vk_pipelineTableSize, VK_KeepUsed );
	}
}

/*
===============
vk_end_prebuild

The registration's shaders have their pipelines (tr_pipelines.c): builds the
keys recorded in earlier runs, and the pipelines built from now on are those
of shaders registered later (VK_ORIGIN_LATE, recorded for the next runs)
===============
*/
void vk_end_prebuild( void ) {
	vkPipelineKey_t key;
	int i;

	if ( !vk.device || !vk_pipelines ) {
		return;
	}
	for ( i = 0; i < vk_numRecordedKeys; i++ ) {
		if ( VK_KeyValid( vk_recordedKeys[i].bits ) ) {
			key.bits = vk_recordedKeys[i].bits;
			key.alphaRef = vk_recordedKeys[i].alphaRef;
			VK_PrebuildKey( &key, VK_ORIGIN_RECORDED );
		}
	}
	vk_prebuildLate = qtrue;
	vk_prebuiltNow = 0;
}

/*
====================================================================

INIT / SHUTDOWN

====================================================================
*/

VkShaderModule VK_CreateShaderModule( const uint32_t *code, size_t size ) {
	VkShaderModuleCreateInfo info;
	VkShaderModule module;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = size;
	info.pCode = code;
	VK_CHECK( qvkCreateShaderModule( vk.device, &info, NULL, &module ) );
	return module;
}

/*
===============
VK_ReadPipelineCache

The data of a pipeline cache saved by the same device and driver, or NULL;
free it with ri.FS_FreeFile
===============
*/
static void *VK_ReadPipelineCache( size_t *size ) {
	const VkPhysicalDeviceProperties *props = &vk.deviceProperties;
	uint32_t header[4];
	void *data;
	int len;

	*size = 0;
	len = ri.FS_ReadFileEx( VK_PIPELINE_CACHE_FILE, &data, qtrue );
	if ( len <= 0 || !data ) {
		return NULL;
	}

	// VkPipelineCacheHeaderVersionOne: length, version, vendor, device, UUID
	if ( len >= 32 ) {
		Com_Memcpy( header, data, sizeof( header ) );
		if ( header[0] >= 32 && header[0] <= (uint32_t)len && header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
			&& header[2] == props->vendorID && header[3] == props->deviceID
			&& !memcmp( (const byte *)data + 16, props->pipelineCacheUUID, VK_UUID_SIZE ) ) {
			*size = len;
			return data;
		}
	}

	ri.Printf( PRINT_ALL, "Vulkan: %s is from another device or driver, ignored\n", VK_PIPELINE_CACHE_FILE );
	ri.FS_FreeFile( data );
	return NULL;
}

static void VK_WritePipelineCache( void ) {
	size_t size;
	void *data;

	// the same size as the data read at startup: nothing new to save
	if ( qvkGetPipelineCacheData( vk.device, vk.pipelineCache, &size, NULL ) != VK_SUCCESS || !size || size == vk_pipelineCacheLoaded ) {
		return;
	}
	data = VK_Alloc( size );
	if ( qvkGetPipelineCacheData( vk.device, vk.pipelineCache, &size, data ) == VK_SUCCESS ) {
		ri.FS_WriteFile( VK_PIPELINE_CACHE_FILE, data, size );
	}
	VK_Free( data );
}

void VK_InitPipelines( void ) {
	VkDescriptorSetLayoutBinding binding, bindings[3];
	VkDescriptorSetLayoutCreateInfo setLayoutInfo;
	VkDescriptorSetLayout setLayouts[3];
	VkDescriptorPoolCreateInfo poolInfo;
	VkDescriptorPoolSize poolSize;
	VkPushConstantRange pushRanges[2];
	VkPipelineLayoutCreateInfo layoutInfo;
	VkPipelineCacheCreateInfo cacheInfo;
	void *cacheData;
	size_t cacheSize;
	int i;

	// one combined image sampler per texture unit, sets 0 and 1
	Com_Memset( &binding, 0, sizeof( binding ) );
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	Com_Memset( &setLayoutInfo, 0, sizeof( setLayoutInfo ) );
	setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutInfo.bindingCount = 1;
	setLayoutInfo.pBindings = &binding;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &setLayoutInfo, NULL, &vk.textureSetLayout ) );

	// set 2, one per frame slot: the dynamic lights of the frame
	// (vk_add_dlights), the skin data and the frame's skin data
	// (vk_add_skin_data, vk_alloc_skin_frame)
	for ( i = 0; i < 3; i++ ) {
		bindings[i] = binding;
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].stageFlags = i ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	setLayoutInfo.bindingCount = ARRAY_LEN( bindings );
	setLayoutInfo.pBindings = bindings;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &setLayoutInfo, NULL, &vk.dlightSetLayout ) );

	poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSize.descriptorCount = VK_MAX_FRAMES * ARRAY_LEN( bindings );
	Com_Memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = VK_MAX_FRAMES;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	VK_CHECK( qvkCreateDescriptorPool( vk.device, &poolInfo, NULL, &vk.dlightPool ) );

	// push constants, see vkPushConstants_t
	pushRanges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pushRanges[0].offset = 0;
	pushRanges[0].size = VK_PUSH_VERTEX_SIZE;
	pushRanges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRanges[1].offset = VK_PUSH_VERTEX_SIZE;
	pushRanges[1].size = VK_PUSH_FRAGMENT_SIZE;

	setLayouts[0] = setLayouts[1] = vk.textureSetLayout;
	setLayouts[2] = vk.dlightSetLayout;
	Com_Memset( &layoutInfo, 0, sizeof( layoutInfo ) );
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = ARRAY_LEN( setLayouts );
	layoutInfo.pSetLayouts = setLayouts;
	layoutInfo.pushConstantRangeCount = ARRAY_LEN( pushRanges );
	layoutInfo.pPushConstantRanges = pushRanges;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &layoutInfo, NULL, &vk.pipelineLayout ) );

	vk.vertexShader = VK_CreateShaderModule( vk_spirv_generic_vert, vk_spirv_generic_vert_size );
	if ( vk.enabledFeatures.shaderClipDistance ) {
		vk.vertexShaderClip = VK_CreateShaderModule( vk_spirv_generic_clip_vert, vk_spirv_generic_clip_vert_size );
	}
	vk.fragmentShader = VK_CreateShaderModule( vk_spirv_generic_frag, vk_spirv_generic_frag_size );
	vk.skinVertexShader = VK_CreateShaderModule( vk_spirv_skinned_vert, vk_spirv_skinned_vert_size );
	if ( vk.enabledFeatures.shaderClipDistance ) {
		vk.skinVertexShaderClip = VK_CreateShaderModule( vk_spirv_skinned_clip_vert, vk_spirv_skinned_clip_vert_size );
	}
	vk.effectsVertexShader = VK_CreateShaderModule( vk_spirv_effects_vert, vk_spirv_effects_vert_size );
	if ( vk.enabledFeatures.shaderClipDistance ) {
		vk.effectsVertexShaderClip = VK_CreateShaderModule( vk_spirv_effects_clip_vert, vk_spirv_effects_clip_vert_size );
	}

	cacheData = VK_ReadPipelineCache( &cacheSize );
	Com_Memset( &cacheInfo, 0, sizeof( cacheInfo ) );
	cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	cacheInfo.initialDataSize = cacheSize;
	cacheInfo.pInitialData = cacheData;
	if ( qvkCreatePipelineCache( vk.device, &cacheInfo, NULL, &vk.pipelineCache ) != VK_SUCCESS && cacheData ) {
		// a driver may refuse data it can't use: start empty
		cacheInfo.initialDataSize = 0;
		cacheInfo.pInitialData = NULL;
		VK_CHECK( qvkCreatePipelineCache( vk.device, &cacheInfo, NULL, &vk.pipelineCache ) );
	}
	vk_pipelineCacheLoaded = cacheInfo.initialDataSize;
	if ( cacheData ) {
		ri.FS_FreeFile( cacheData );
	}

	vk_pipelineTableSize = 256;
	vk_pipelines = VK_Alloc( sizeof( *vk_pipelines ) * vk_pipelineTableSize );
	Com_Memset( vk_pipelines, 0, sizeof( *vk_pipelines ) * vk_pipelineTableSize );
	vk_numPipelines = 0;
	vk_prebuildLate = qfalse;
	vk_prebuiltNow = 0;
	VK_ReadRecordedKeys();
}

void VK_ShutdownPipelines( void ) {
	int i;

	if ( vk_pipelines ) {
		VK_WriteRecordedKeys();
		for ( i = 0; i < vk_pipelineTableSize; i++ ) {
			if ( vk_pipelines[i].pipeline ) {
				qvkDestroyPipeline( vk.device, vk_pipelines[i].pipeline, NULL );
			}
		}
		VK_Free( vk_pipelines );
		vk_pipelines = NULL;
	}
	vk_pipelineTableSize = 0;
	vk_numPipelines = 0;

	if ( vk.pipelineCache ) {
		VK_WritePipelineCache();
		qvkDestroyPipelineCache( vk.device, vk.pipelineCache, NULL );
	}
	if ( vk.vertexShader ) {
		qvkDestroyShaderModule( vk.device, vk.vertexShader, NULL );
	}
	if ( vk.vertexShaderClip ) {
		qvkDestroyShaderModule( vk.device, vk.vertexShaderClip, NULL );
	}
	if ( vk.fragmentShader ) {
		qvkDestroyShaderModule( vk.device, vk.fragmentShader, NULL );
	}
	if ( vk.skinVertexShader ) {
		qvkDestroyShaderModule( vk.device, vk.skinVertexShader, NULL );
	}
	if ( vk.skinVertexShaderClip ) {
		qvkDestroyShaderModule( vk.device, vk.skinVertexShaderClip, NULL );
	}
	if ( vk.effectsVertexShader ) {
		qvkDestroyShaderModule( vk.device, vk.effectsVertexShader, NULL );
	}
	if ( vk.effectsVertexShaderClip ) {
		qvkDestroyShaderModule( vk.device, vk.effectsVertexShaderClip, NULL );
	}
	if ( vk.pipelineLayout ) {
		qvkDestroyPipelineLayout( vk.device, vk.pipelineLayout, NULL );
	}
	if ( vk.textureSetLayout ) {
		qvkDestroyDescriptorSetLayout( vk.device, vk.textureSetLayout, NULL );
	}
	if ( vk.dlightSetLayout ) {
		qvkDestroyDescriptorSetLayout( vk.device, vk.dlightSetLayout, NULL );
	}
	if ( vk.dlightPool ) {
		qvkDestroyDescriptorPool( vk.device, vk.dlightPool, NULL );
	}
	vk.pipelineCache = VK_NULL_HANDLE;
	vk.vertexShader = vk.vertexShaderClip = vk.fragmentShader = VK_NULL_HANDLE;
	vk.skinVertexShader = vk.skinVertexShaderClip = VK_NULL_HANDLE;
	vk.effectsVertexShader = vk.effectsVertexShaderClip = VK_NULL_HANDLE;
	vk.pipelineLayout = VK_NULL_HANDLE;
	vk.textureSetLayout = VK_NULL_HANDLE;
	vk.dlightSetLayout = VK_NULL_HANDLE;
	vk.dlightPool = VK_NULL_HANDLE;
}
