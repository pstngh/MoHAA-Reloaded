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

// tr_pipelines.c -- the pipelines of a map's draws, created while it loads
//
// The back end creates a pipeline the first time a draw needs its state
// (vk_draw, vk_pipeline.c): with cold driver caches (the first play after an
// install or a driver update) that took 1 to 2 ms each on RADV, 25 to 52 ms
// in the first frames after a load and more during a match. At the end of
// the registration (RE_EndRegistration) R_PrebuildPipelines works out the
// draws the stage iterators of tr_shade.c will make with each registered
// shader and creates their pipelines (vk_prebuild_pipeline: the same key,
// so the same pipeline); a shader registered later (an effect or a sprite
// announced in a match) gets its own when it is created. What a draw's key
// holds comes from the shader's stages, as the iterators pass them to
// R_State, R_Cull, R_TexEnv and R_Bind, and from what changes at run
// time, for which the values that can occur are built:
//  - where the shader is drawn: in 2D (RB_StretchPic: depth test off unless
//    the stage has a second texture; the cull Set2DWindow leaves, none, or
//    the shader's), in a 3D view on the CPU (colors per vertex, a constant
//    for RB_StageConstantColor's stages and the lightmapped multitexture
//    iterator), from the static geometry (colors per vertex or a
//    constant), from it with the
//    effects shader (deforms, tcGen and tcMod, distFade, static models'
//    dynamic lights: tr_staticgeom.c), skinned (the vertex-lit iterator and
//    the generic one's stages that R_SkinPasses takes, tr_skin.c);
//  - fog: off, or the stage's GLS_FOG_ENABLED (R_State keeps the fog of
//    the previous state when the bit doesn't change, and Set2DWindow turns
//    it off behind its back, so a fogged stage can draw without it); the
//    map's fog isn't known before its first frame, so both;
//  - dynamic lights on a lightmap unit (r_fastdlights 0, vk_set_lightmap_
//    dlights), with r_dlightBacks 0's test when it is set, and with
//    r_fastdlights 1 the projected pass (ProjectDlightTexture) of opaque
//    world shaders;
//  - every image an animated bundle binds (RGB textures change the key);
//  - with the temporal path, blended stages as reactive effects or not
//    (vk_set_reactive);
//  - the sky box and the immediate mode draws that come in every frame
//    (the color clear drawn as a quad, the UI's boxes), after whatever cull
//    the draw before left.
// Not built: mirror and portal views (the cull swapped, a clip plane),
// debug drawing, stencil shadows, r_measureOverdraw and the other immediate
// mode draws: they are created at their first draw, and vk_pipeline.c
// records the keys created after a load (VK_PIPELINE_KEYS_FILE) and builds
// them at the end of the next runs' registrations.

#include "tr_local.h"

static qboolean	r_prebuilt;		// the registration's pipelines are built: new shaders get theirs

// the fog of the draws built: at load both (the map's fog isn't known yet),
// for a shader registered later that of the last 3D view
typedef enum {
	PB_FOG_BOTH,
	PB_FOG_ON,
	PB_FOG_OFF
} prebuildFog_t;

static prebuildFog_t	r_prebuildFog;
static int				r_prebuildSetupFogs;	// backEnd.setupFogs at the end of the registration

/*
===============
R_StateTexEnv

The texture environment R_State gives the current unit for these state bits
===============
*/
static texEnv_t R_StateTexEnv( unsigned long stateBits ) {
	if ( stateBits & GLS_MULTITEXTURE_ENV ) {
		return TEXENV_COMBINE4;
	}
	if ( stateBits & GLS_MULTITEXTURE ) {
		return TEXENV_COMBINE_INTERPOLATE;
	}
	return TEXENV_MODULATE;
}

// world surfaces (faces, grids, terrain, brush models) have a lightmap or
// vertex colors, the models' surfaces (R_RegisterShaders) none; what
// RE_RegisterShader and RE_RegisterShaderNoMip register (the 2D pictures,
// and cgame's effects, sprites and marks) is LIGHTMAP_2D
static qboolean R_ShaderIsWorld( const shader_t *shader ) {
	return shader->lightmapIndex >= 0 || shader->lightmapIndex == LIGHTMAP_BY_VERTEX || shader->lightmapIndex == LIGHTMAP_WHITEIMAGE;
}

static cullFace_t R_ShaderCull( const shader_t *shader ) {
	switch ( shader->cullType ) {
	case CT_TWO_SIDED:
		return CULL_NONE;
	case CT_BACK_SIDED:
		return CULL_BACK;
	default:
		return CULL_FRONT;
	}
}

/*
===============
R_PrebuildFog

The draw without fog, and with it when the state's GLS_FOG_ENABLED asks for
it: R_State turns the fog on for such a state in a fogged view, off for
the others, and keeps it when the bit doesn't change (Set2DWindow turns it
off behind its back)
===============
*/
static int R_PrebuildFog( vkDrawState_t *ds, unsigned long fogBits ) {
	int n = 0;

	if ( r_prebuildFog != PB_FOG_ON || !( fogBits & GLS_FOG_ENABLED ) ) {
		ds->fog = qfalse;
		n += vk_prebuild_pipeline( ds );
	}
	if ( r_prebuildFog != PB_FOG_OFF && ( fogBits & GLS_FOG_ENABLED ) ) {
		ds->fog = qtrue;
		n += vk_prebuild_pipeline( ds );
	}
	return n;
}

/*
===============
R_PrebuildImages

The draw with each image the bundles of its units can bind
(R_BindAnimatedImage): their RGB textures change the key
===============
*/
static int R_PrebuildImages( vkDrawState_t *ds, const textureBundle_t *b0, const textureBundle_t *b1, unsigned long fogBits ) {
	int n0 = b0 && b0->numImageAnimations > 1 ? b0->numImageAnimations : 1;
	int n1 = b1 && b1->numImageAnimations > 1 ? b1->numImageAnimations : 1;
	int i, j, n = 0;

	for ( i = 0; i < n0; i++ ) {
		if ( b0 ) {
			ds->image[0] = b0->image[i] ? b0->image[i] : tr.defaultImage;
		}
		for ( j = 0; j < n1; j++ ) {
			if ( b1 ) {
				ds->image[1] = b1->image[j] ? b1->image[j] : tr.defaultImage;
			}
			n += R_PrebuildFog( ds, fogBits );
		}
	}
	return n;
}

/*
===============
R_StageMayBeConstant

Whether RB_StaticStageColor can give the stage a constant color (a draw
from the static geometry without colors per vertex) in some batch: its
rgbGen and alphaGen don't need the vertex colors or the CPU (with the
vertex alpha of static models, always 255, too). Colors per vertex give
the key of the CPU's draws
===============
*/
static qboolean R_StageMayBeConstant( const shaderStage_t *pStage ) {
	switch ( pStage->rgbGen ) {
	case CGEN_EXACT_VERTEX:
	case CGEN_VERTEX:
	case CGEN_LIGHTING_SPHERICAL:
	case CGEN_STATIC:
	case CGEN_ONE_MINUS_VERTEX:
	case CGEN_LIGHTING_GRID:
	case CGEN_SCOORD:
	case CGEN_TCOORD:
	case CGEN_DOT:
	case CGEN_ONE_MINUS_DOT:
		return qfalse;
	default:
		break;
	}
	switch ( pStage->alphaGen ) {
	case AGEN_ONE_MINUS_VERTEX:
	case AGEN_LIGHTING_SPECULAR:
	case AGEN_PORTAL:
	case AGEN_DOT:
	case AGEN_ONE_MINUS_DOT:
	case AGEN_SCOORD:
	case AGEN_TCOORD:
	case AGEN_DIST_FADE:
	case AGEN_ONE_MINUS_DIST_FADE:
	case AGEN_DOT_VIEW:
	case AGEN_ONE_MINUS_DOT_VIEW:
	case AGEN_HEIGHT_FADE:
		return qfalse;
	default:
		return qtrue;
	}
}

/*
===============
R_StageNeedsEffects

Whether a stage can need the effects shader to be drawn from the static
geometry in some batch (RB_StaticBatchAllowed): the shader's deforms, the
texture coordinates of a bundle, distFade, or the lighting of a static
model with dynamic lights near it
===============
*/
static qboolean R_StageNeedsEffects( const shader_t *shader, const shaderStage_t *pStage, qboolean vertexLit ) {
	int b;

	if ( shader->numDeforms ) {
		return !vertexLit && RB_StaticFxDeforms( shader );
	}
	if ( pStage->rgbGen == CGEN_LIGHTING_SPHERICAL || pStage->rgbGen == CGEN_STATIC
		|| pStage->alphaGen == AGEN_DIST_FADE || pStage->alphaGen == AGEN_ONE_MINUS_DIST_FADE ) {
		return qtrue;
	}
	if ( vertexLit ) {
		return qfalse;
	}
	for ( b = 0; b < NUM_TEXTURE_BUNDLES && pStage->bundle[b].image[0]; b++ ) {
		const textureBundle_t *bundle = &pStage->bundle[b];

		if ( ( ( bundle->numTexMods && bundle->texMods[0].type != TMOD_NONE )
			|| ( bundle->tcGen != TCGEN_TEXTURE && bundle->tcGen != TCGEN_LIGHTMAP ) ) && RB_StaticFxBundle( bundle, qtrue ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
===============
R_StageMayBeEffects

Whether a stage can be drawn with the effects shader: when it or another
stage of its shader needs it, as every draw of such a batch is
(RB_StaticDraw)
===============
*/
static qboolean R_StageMayBeEffects( const shader_t *shader, const shaderStage_t *pStage, qboolean vertexLit ) {
	int i;

	// the vertex-lit iterator draws one stage
	if ( vertexLit ) {
		return R_StageNeedsEffects( shader, pStage, qtrue );
	}
	for ( i = 0; i < MAX_SHADER_STAGES && shader->unfoggedStages[i]; i++ ) {
		if ( R_StageNeedsEffects( shader, shader->unfoggedStages[i], qfalse ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

// where a shader is drawn
#define PB_2D			1	// by the 2D calls
#define PB_CPU			2	// in a 3D view, through tess on the CPU: colors per vertex
#define PB_STATIC		4	// from the static geometry: colors per vertex or a constant
#define PB_SKINNED		8	// skinned on the GPU (tr_skin.c)
#define PB_EFFECTS		16	// from the static geometry with the effects shader

/*
===============
R_PrebuildStage

The draws of a stage drawn by the generic iterator (RB_IterateStagesGeneric)
or the vertex-lit one, in each place, with the shader's cull and polygon
offset; dlights: lightmap stages may get the dynamic lights per pixel
===============
*/
static int R_PrebuildStage( const shader_t *shader, shaderStage_t *pStage, int where, qboolean vertexLit ) {
	const qboolean multitexture = !vertexLit && pStage->bundle[1].image[0] != NULL;
	vkDrawState_t ds;
	int place, n = 0;

	for ( place = PB_2D; place <= PB_EFFECTS; place <<= 1 ) {
		int dlightTmu = -1, cull, numCulls, lights, clamp, numClamps;

		if ( !( where & place ) ) {
			continue;
		}
		// the static geometry's draws have colors per vertex (the CPU's key)
		// or a constant
		if ( place == PB_STATIC && !R_StageMayBeConstant( pStage ) ) {
			continue;
		}
		if ( place == PB_EFFECTS && !R_StageMayBeEffects( shader, pStage, vertexLit ) ) {
			continue;
		}

		Com_Memset( &ds, 0, sizeof( ds ) );
		ds.prim = PRIM_TRIANGLES;
		ds.stateBits = pStage->stateBits;
		ds.texEnv[0] = R_StateTexEnv( pStage->stateBits );
		ds.texEnv[1] = TEXENV_MODULATE;
		ds.skinned = place == PB_SKINNED;
		ds.effects = place == PB_EFFECTS;
		ds.colorArray = place != PB_STATIC && place != PB_EFFECTS && ( place == PB_SKINNED || !RB_StageConstantColor( pStage ) );
		// the generic iterator sets the shader's (the vertex-lit one is
		// only chosen without it)
		ds.polygonOffset = shader->polygonOffset && !vertexLit;
		// the temporal path's reactive effects (RB_TemporalReactive): a
		// blended stage of a 3D view whose shader changes over time always
		// is; one of the world's is not (on a brush model entity it is: the
		// draw creates that one); one of a model or an effect can be either
		// (an entity's, a static model's)
		if ( place != PB_2D && ( pStage->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) && !ds.polygonOffset ) {
			ds.reactive = RB_ShaderAnimated( (shader_t *)shader ) ? 1 : R_ShaderIsWorld( shader ) ? 0 : 2;
		}

		if ( multitexture ) {
			// DrawMultitextured: no GLS_DEPTHTEST_DISABLE in 2D
			if ( r_lightmap->integer && pStage->bundle[1].isLightmap ) {
				ds.texEnv[1] = TEXENV_REPLACE;
			} else {
				ds.texEnv[1] = pStage->multitextureEnv;
			}
			if ( pStage->bundle[1].isLightmap ) {
				dlightTmu = 1;
			}
		} else {
			if ( place == PB_2D ) {
				ds.stateBits |= GLS_DEPTHTEST_DISABLE;
			}
			if ( pStage->bundle[0].isLightmap && !( pStage->bundle[0].vertexLightmap && r_vertexLight->integer && r_lightmap->integer ) ) {
				dlightTmu = 0;
			}
		}

		// depth clamp in 3D views (RB_DrawElements); a model can also be
		// the view weapon, whose depth hack has none
		numClamps = 1;
		ds.depthClamp = place != PB_2D && RB_ShaderDepthClamp( shader );
		// (level 1: only world entity draws are clamped, other entities and
		// static models are not)
		if ( ds.depthClamp && ( r_depthClamp->integer < 2 || ( !R_ShaderIsWorld( shader ) && ( place == PB_CPU || place == PB_SKINNED ) ) ) ) {
			numClamps = 2;
		}

		// Set2DWindow sets no cull behind R_Cull's back
		numCulls = place == PB_2D && R_ShaderCull( shader ) != CULL_NONE ? 2 : 1;
		for ( clamp = 0; clamp < numClamps; clamp++ ) {
			if ( clamp ) {
				ds.depthClamp = qfalse;
			}
			for ( cull = 0; cull < numCulls; cull++ ) {
				ds.cull = cull ? CULL_NONE : R_ShaderCull( shader );
				for ( lights = 0; lights < ( dlightTmu >= 0 && place != PB_2D ? 3 : 1 ); lights++ ) {
					if ( lights == 2 && r_dlightBacks->integer ) {
						break;
					}
					ds.dlightTmu = lights ? dlightTmu : -1;
					ds.dlightBacks = lights == 2;

					if ( !multitexture && pStage->bundle[0].vertexLightmap && r_vertexLight->integer && r_lightmap->integer ) {
						ds.image[0] = tr.whiteImage;
						n += R_PrebuildFog( &ds, pStage->stateBits );
					} else {
						n += R_PrebuildImages( &ds, &pStage->bundle[0], multitexture ? &pStage->bundle[1] : NULL, pStage->stateBits );
					}
				}
			}
		}
	}
	return n;
}

/*
===============
R_PrebuildLightmappedMultitexture

RB_StageIteratorLightmappedMultitextureUnfogged: one stage, its alpha test
and fog bits only, the lightmap modulated by unit 1, white as the constant
color (on the CPU and from the static geometry)
===============
*/
static int R_PrebuildLightmappedMultitexture( const shader_t *shader ) {
	shaderStage_t *pStage = shader->unfoggedStages[0];
	vkDrawState_t ds;
	int lights, n = 0;

	Com_Memset( &ds, 0, sizeof( ds ) );
	ds.prim = PRIM_TRIANGLES;
	ds.stateBits = GLS_DEFAULT | ( pStage->stateBits & ( GLS_ATEST_BITS | GLS_FOG_BITS ) );
	ds.texEnv[0] = R_StateTexEnv( ds.stateBits );
	ds.texEnv[1] = r_lightmap->integer ? TEXENV_REPLACE : TEXENV_MODULATE;
	ds.cull = R_ShaderCull( shader );
	ds.depthClamp = RB_ShaderDepthClamp( shader );

	for ( lights = 0; lights < ( pStage->bundle[1].isLightmap ? 3 : 1 ); lights++ ) {
		if ( lights == 2 && r_dlightBacks->integer ) {
			break;
		}
		ds.dlightTmu = lights ? 1 : -1;
		ds.dlightBacks = lights == 2;
		n += R_PrebuildImages( &ds, &pStage->bundle[0], &pStage->bundle[1], ds.stateBits );
	}
	return n;
}

/*
===============
R_PrebuildDlightPass

ProjectDlightTexture after an opaque shader's stages: additive or
modulated, with the iterator's cull and polygon offset, depth equal
===============
*/
static int R_PrebuildDlightPass( const shader_t *shader ) {
	static const unsigned long stateBits[2] = {
		GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL,
		GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL
	};
	vkDrawState_t ds;
	int i, n = 0;

	if ( !tr.dlightImage ) {
		return 0;
	}

	Com_Memset( &ds, 0, sizeof( ds ) );
	ds.prim = PRIM_TRIANGLES;
	ds.cull = R_ShaderCull( shader );
	ds.polygonOffset = shader->polygonOffset && shader->optimalStageIteratorFunc == RB_StageIteratorGeneric;
	ds.texEnv[0] = TEXENV_MODULATE;
	ds.texEnv[1] = TEXENV_MODULATE;
	ds.image[0] = tr.dlightImage;
	ds.dlightTmu = -1;
	ds.colorArray = qtrue;
	ds.depthClamp = RB_ShaderDepthClamp( shader );
	for ( i = 0; i < 2; i++ ) {
		ds.stateBits = stateBits[i];
		n += R_PrebuildFog( &ds, stateBits[i] );
	}
	return n;
}

/*
===============
R_PrebuildSkyBox

RB_StageIteratorSky's outer box: R_State( 0 ), immediate mode (a constant
color), after whatever cull and fog the previous draw left
===============
*/
static int R_PrebuildSkyBox( const shader_t *shader ) {
	vkDrawState_t ds;
	int i, cull, n = 0;

	if ( !shader->sky.outerbox[0] || shader->sky.outerbox[0] == tr.defaultImage ) {
		return 0;
	}

	Com_Memset( &ds, 0, sizeof( ds ) );
	ds.prim = PRIM_TRIANGLES;
	ds.texEnv[0] = TEXENV_MODULATE;
	ds.texEnv[1] = TEXENV_MODULATE;
	ds.dlightTmu = -1;
	for ( cull = CULL_NONE; cull <= CULL_BACK; cull++ ) {
		ds.cull = cull;
		for ( i = 0; i < 6; i++ ) {
			ds.image[0] = shader->sky.outerbox[i] ? shader->sky.outerbox[i] : tr.defaultImage;
			n += R_PrebuildFog( &ds, GLS_FOG_ENABLED );
		}
	}
	return n;
}

/*
===============
R_PrebuildShader

The pipelines of the draws of one shader; returns how many were created
===============
*/
static int R_PrebuildShader( const shader_t *shader ) {
	int where, i, n = 0;
	qboolean world;

	if ( !shader ) {
		return 0;
	}
	if ( shader->isSky ) {
		n += R_PrebuildSkyBox( shader );
	}
	if ( !shader->numUnfoggedPasses || !shader->unfoggedStages[0] ) {
		return n;
	}

	world = R_ShaderIsWorld( shader );
	if ( shader->lightmapIndex == LIGHTMAP_2D ) {
		where = PB_2D | PB_CPU;
	} else {
		where = PB_CPU;
		// the static geometry: world surfaces and static models (registered
		// with the map), not the sky; deforms only with the effects shader
		if ( !shader->isSky && !r_prebuilt ) {
			if ( !shader->numDeforms ) {
				where |= PB_STATIC;
			}
			if ( r_staticEffects->integer && vk_effects_available() ) {
				where |= PB_EFFECTS;
			}
		}
	}

	if ( shader->optimalStageIteratorFunc == RB_StageIteratorLightmappedMultitextureUnfogged ) {
		n += R_PrebuildLightmappedMultitexture( shader );
	} else if ( shader->optimalStageIteratorFunc == RB_StageIteratorVertexLitTextureUnfogged ) {
		if ( !world && r_gpuSkinning->integer ) {
			where |= PB_SKINNED;
		}
		n += R_PrebuildStage( shader, shader->unfoggedStages[0], where & ~PB_2D, qtrue );
	} else {
		// generic, also the sky's cloud stages; skinned when every stage
		// can be (models' surfaces, custom shaders)
		if ( !world && r_gpuSkinning->integer && R_SkinPasses( shader ) ) {
			where |= PB_SKINNED;
		}
		for ( i = 0; i < MAX_SHADER_STAGES && shader->unfoggedStages[i]; i++ ) {
			n += R_PrebuildStage( shader, shader->unfoggedStages[i], shader->isSky ? PB_CPU : where, qfalse );
			if ( r_lightmap->integer && ( shader->unfoggedStages[i]->bundle[0].isLightmap || shader->unfoggedStages[i]->bundle[1].isLightmap
				|| shader->unfoggedStages[i]->bundle[0].vertexLightmap ) ) {
				break;
			}
		}
	}

	if ( r_fastdlights->integer && world && shader->sort <= SS_OPAQUE && !shader->isSky ) {
		n += R_PrebuildDlightPass( shader );
	}
	return n;
}

/*
===============
R_PrebuildImmediate

What the back end draws in immediate mode whatever the shaders: the color
clear drawn as a quad (VK_ClearColor: the 3D view of fogged maps, cleared to
the fog color) and the UI's boxes (DrawBox, AddBox: no texture, a constant
color, the cull of the draw before, the fog that R_State turns off)
===============
*/
static int R_PrebuildImmediate( void ) {
	static const unsigned long boxes[2] = {
		GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA,
		GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE
	};
	vkDrawState_t ds;
	int i, cull, n = 0;

	Com_Memset( &ds, 0, sizeof( ds ) );
	ds.prim = PRIM_TRIANGLES;
	ds.texEnv[0] = ds.texEnv[1] = TEXENV_MODULATE;
	ds.dlightTmu = -1;
	ds.stateBits = GLS_DEPTHTEST_DISABLE;
	ds.cull = CULL_NONE;
	n += vk_prebuild_pipeline( &ds );
	for ( i = 0; i < 2; i++ ) {
		ds.stateBits = boxes[i];
		for ( cull = CULL_NONE; cull <= CULL_BACK; cull++ ) {
			ds.cull = cull;
			n += vk_prebuild_pipeline( &ds );
		}
	}
	return n;
}

/*
===============
R_PrebuildPipelines

At the end of the registration: the pipelines of every registered shader
===============
*/
void R_PrebuildPipelines( void ) {
	int start = ri.Milliseconds();
	int i, n = 0;

	r_prebuildFog = PB_FOG_BOTH;
	for ( i = 0; i < tr.numShaders; i++ ) {
		n += R_PrebuildShader( tr.shaders[i] );
	}
	n += R_PrebuildImmediate();
	vk_end_prebuild();
	r_prebuilt = qtrue;
	r_prebuildSetupFogs = backEnd.setupFogs;

	ri.Printf( PRINT_DEVELOPER, "R_PrebuildPipelines: %d shaders, %d pipelines created, %d ms\n",
		tr.numShaders, n, ri.Milliseconds() - start );
}

/*
===============
R_PrebuildNewShader

A shader created after the registration's pipelines were built (a model or
an effect announced in the match): in the frame that registers it, so only
with the fog of the last 3D view (RB_SetupFog) once there was one, and not
for the static geometry
===============
*/
void R_PrebuildNewShader( const shader_t *shader ) {
	if ( r_prebuilt ) {
		if ( backEnd.setupFogs == r_prebuildSetupFogs ) {
			r_prebuildFog = PB_FOG_BOTH;
		} else {
			r_prebuildFog = ( glState.externalSetState & GLS_FOG ) ? PB_FOG_ON : PB_FOG_OFF;
		}
		R_PrebuildShader( shader );
	}
}

/*
===============
R_PrebuildReset

A new registration: its shaders are built at its end
===============
*/
void R_PrebuildReset( void ) {
	r_prebuilt = qfalse;
	vk_begin_prebuild();
}
