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

// tr_skin.c -- skeletal models skinned and lit on the GPU
//
// RB_SkelMesh computed every vertex of a skeletal model's surfaces from its
// bones (positions and normals, weights and morphs) on the CPU, and the
// vertex-lit stage iterator then lit them per vertex (RB_Light_Real, rgbGen
// lightingSpherical), every frame: most of the frame where players are in
// view. A batch whose shader takes that path (one stage, rgbGen
// lightingSpherical or lightingGrid, RB_StageIteratorVertexLitTextureUnfogged)
// is now drawn with shaders/skinned.vert instead, which does both with the
// same floating point operations: the vertexes of each surface are added
// once to the skin data (vk_add_skin_data), and what changes every frame,
// the entity's bones and morph weights and its sphere's lights (or the
// constant color of the other lighting functions), goes into the frame data
// (vk_alloc_skin_frame), once per entity and frame. So is a batch of the
// generic iterator whose stages are all one texture with the mesh's texture
// coordinates or an environment map, lit by the sphere or with a color that
// doesn't depend on the vertex (helmets, weapons with a shine pass: from
// JayRewd's a5f5a54a): each stage is one draw per surface, with its own
// draw records (R_SkinPasses, RB_SkinStage). Batches stay the same:
// a skinned batch counts the vertexes and indexes it would have put into
// tess, so it is split at the same surfaces, and draws each surface (with
// the indexes of its level of detail) in the same order. Everything else
// (other shaders, debug drawing, lod_tool, no room left in the skin or
// frame data) is skinned on the CPU as before.

#include "tr_local.h"
#include "../tiki/tiki_shared.h"
#include "../tiki/tiki_mesh.h"

cvar_t *r_gpuSkinning;

/*
====================================================================

SKIN DATA

What was added to the skin data for each skeletal surface and TIKI,
found by pointer; forgotten at each map load (the models may be freed)

====================================================================
*/

typedef enum {
	SKIN_SURFACE,	// value: first vertex record, first word of the indexes
	SKIN_TIKI,		// value: whether all of it is in the skin data, the morph weights it reads
	SKIN_REMAP		// plus the mesh, one after the first of a TIKI: first word of its bone remap table
} skinKind_t;

typedef struct {
	const void	*key;
	int			kind;		// skinKind_t, plus the mesh for SKIN_REMAP
	int			value[2];
	int			*thresholds;	// SKIN_SURFACE: see R_SkinThresholds
} skinEntry_t;

static skinEntry_t	*skinTable;
static int			skinTableSize, skinTableUsed;

static unsigned int R_SkinHash( const void *key, int kind ) {
	uintptr_t k = (uintptr_t)key;

	return (unsigned int)( ( k >> 4 ) * 2654435761u ) ^ (unsigned int)kind * 40503u;
}

static skinEntry_t *R_SkinFind( const void *key, int kind ) {
	unsigned int i;

	if ( !skinTable ) {
		return NULL;
	}
	for ( i = R_SkinHash( key, kind ) & ( skinTableSize - 1 ); skinTable[i].key; i = ( i + 1 ) & ( skinTableSize - 1 ) ) {
		if ( skinTable[i].key == key && skinTable[i].kind == kind ) {
			return &skinTable[i];
		}
	}
	return NULL;
}

static skinEntry_t *R_SkinInsert( const void *key, int kind, int a, int b ) {
	unsigned int i;

	if ( 2 * ( skinTableUsed + 1 ) > skinTableSize ) {
		skinEntry_t *old = skinTable;
		int oldSize = skinTableSize, j;

		skinTableSize = oldSize ? oldSize * 2 : 1024;
		skinTable = ri.Malloc( sizeof( skinEntry_t ) * skinTableSize );
		Com_Memset( skinTable, 0, sizeof( skinEntry_t ) * skinTableSize );
		skinTableUsed = 0;
		for ( j = 0; j < oldSize; j++ ) {
			if ( old[j].key ) {
				R_SkinInsert( old[j].key, old[j].kind, old[j].value[0], old[j].value[1] )->thresholds = old[j].thresholds;
			}
		}
		if ( old ) {
			ri.Free( old );
		}
	}

	for ( i = R_SkinHash( key, kind ) & ( skinTableSize - 1 ); skinTable[i].key; i = ( i + 1 ) & ( skinTableSize - 1 ) ) {
	}
	skinTable[i].key = key;
	skinTable[i].kind = kind;
	skinTable[i].value[0] = a;
	skinTable[i].value[1] = b;
	skinTable[i].thresholds = NULL;
	skinTableUsed++;
	return &skinTable[i];
}

/*
===============
R_ClearSkinData

A new map, or a new backend (vid_restart): nothing is in the skin data
===============
*/
void R_ClearSkinData( void ) {
	int i;

	if ( skinTable ) {
		for ( i = 0; i < skinTableSize; i++ ) {
			if ( skinTable[i].thresholds ) {
				ri.Free( skinTable[i].thresholds );
			}
		}
		ri.Free( skinTable );
	}
	skinTable = NULL;
	skinTableSize = skinTableUsed = 0;
	vk_clear_skin_data();
	// the effects shader's wave tables
	R_ClearFxTables();
	// the meshes of the skeletal surfaces (RB_SkelMesh)
	R_ClearSkelMeshes();
}

/*
===============
R_SkinCollapse

What RB_SkelMesh's collapse map makes of vertex v at level of detail
render_count (all the vertexes from render_count on are collapsed)
===============
*/
static int R_SkinCollapse( const skelIndex_t *collapse_map, int v, int render_count ) {
	while ( v >= render_count ) {
		v = collapse_map[v];
	}
	return v;
}

static qboolean R_SkinDegenerate( const skelIndex_t *tri, const skelIndex_t *collapse_map, int render_count ) {
	int a = R_SkinCollapse( collapse_map, tri[0], render_count );
	int b = R_SkinCollapse( collapse_map, tri[1], render_count );
	int c = R_SkinCollapse( collapse_map, tri[2], render_count );

	return a == b || b == c || c == a;
}

/*
===============
R_SkinThresholds

At a lower level of detail (render_count below numVerts), RB_SkelMesh keeps
the triangles up to the first one that its collapse map makes degenerate.
A triangle that is degenerate at a level of detail is at every lower one:
following a vertex's collapse chain from render_count down only reaches
vertexes further down the chains, and two chains that met stay together.
So each triangle has a highest level at which it is degenerate (2 when it
never is), and the triangles RB_SkelMesh keeps at render_count are those
before the first whose threshold reaches it: entry t of the array is the
largest threshold of the triangles up to t, found with a binary search.
NULL when a vertex's collapse target isn't below it: RB_SkelMesh then reads
entries of its collapse array it hasn't filled (the colt45 has an identity
map), so its indexes are computed as it does
===============
*/
static int *R_SkinThresholds( const skelSurfaceGame_t *sf ) {
	const skelIndex_t *collapse_map = sf->pCollapse;
	int *thresholds, highest, i, t;

	if ( !collapse_map || !sf->pTriangles || sf->numVerts <= 3 || sf->numTriangles <= 0 ) {
		return NULL;
	}
	for ( i = 3; i < sf->numVerts; i++ ) {
		if ( collapse_map[i] >= i ) {
			return NULL;
		}
	}

	thresholds = ri.Malloc( sizeof( int ) * sf->numTriangles );
	highest = 2;
	for ( t = 0; t < sf->numTriangles; t++ ) {
		const skelIndex_t *tri = sf->pTriangles + t * 3;
		int lo = 2, hi = sf->numVerts - 1;

		// the largest render_count from 3 to numVerts - 1 at which it is degenerate
		if ( R_SkinDegenerate( tri, collapse_map, 3 ) ) {
			lo = 3;
			while ( lo < hi ) {
				int mid = ( lo + hi + 1 ) >> 1;

				if ( R_SkinDegenerate( tri, collapse_map, mid ) ) {
					lo = mid;
				} else {
					hi = mid - 1;
				}
			}
		}
		highest = Q_max( highest, lo );
		thresholds[t] = highest;
	}
	return thresholds;
}

/*
===============
R_SkinAddSurface

Vertex records, morphs and weights (see vk.h, SKINNED DRAWS), then the
indexes of all the triangles; returns the largest morph target it reads
plus one, or -1 when there was no room
===============
*/
static int R_SkinAddSurface( const skelSurfaceGame_t *sf ) {
	const skeletorVertex_t *v;
	uint32_t *words, *rec, *data, *idx;
	int numData, numWords, first, numMorphTargets, i, j;

	if ( R_SkinFind( sf, SKIN_SURFACE ) ) {
		return 0;
	}

	numData = 0;
	v = sf->pVerts;
	for ( i = 0; i < sf->numVerts; i++ ) {
		numData += v->numMorphs * 4 + v->numWeights * 5;
		v = (const skeletorVertex_t *)( (const byte *)v + sizeof( skeletorVertex_t )
			+ sizeof( skeletorMorph_t ) * v->numMorphs + sizeof( skelWeight_t ) * v->numWeights );
	}
	numWords = sf->numVerts * 8 + numData + sf->numTriangles * 3;
	words = ri.Hunk_AllocateTempMemory( sizeof( uint32_t ) * Q_max( numWords, 1 ) );

	rec = words;
	data = words + sf->numVerts * 8;
	idx = data + numData;
	numMorphTargets = 0;
	v = sf->pVerts;
	for ( i = 0; i < sf->numVerts; i++, rec += 8 ) {
		const skeletorMorph_t *morph = (const skeletorMorph_t *)( v + 1 );
		const skelWeight_t *weight = (const skelWeight_t *)( morph + v->numMorphs );

		Com_Memcpy( rec, v->normal, sizeof( vec3_t ) );
		Com_Memcpy( rec + 3, v->texCoords, sizeof( vec2_t ) );
		rec[5] = (uint32_t)v->numWeights | (uint32_t)v->numMorphs << 16;
		rec[6] = (uint32_t)( data - rec );
		rec[7] = sf->pCollapse ? sf->pCollapse[i] : i;
		for ( j = 0; j < v->numMorphs; j++, data += 4 ) {
			data[0] = morph[j].morphIndex;
			Com_Memcpy( data + 1, morph[j].offset, sizeof( vec3_t ) );
			numMorphTargets = Q_max( numMorphTargets, morph[j].morphIndex + 1 );
		}
		for ( j = 0; j < v->numWeights; j++, data += 5 ) {
			data[0] = weight[j].boneIndex;
			Com_Memcpy( data + 1, &weight[j].boneWeight, sizeof( float ) );
			Com_Memcpy( data + 2, weight[j].offset, sizeof( vec3_t ) );
		}
		v = (const skeletorVertex_t *)( weight + v->numWeights );
	}
	for ( i = 0; i < sf->numTriangles * 3; i++ ) {
		idx[i] = sf->pTriangles[i];
	}

	first = vk_add_skin_data( words, numWords );
	ri.Hunk_FreeTempMemory( words );
	if ( first < 0 ) {
		return -1;
	}

	R_SkinInsert( sf, SKIN_SURFACE, first / 8, first + sf->numVerts * 8 + numData )->thresholds = R_SkinThresholds( sf );
	return numMorphTargets;
}

/*
===============
R_SkinTiki

Adds the surfaces of every mesh of a TIKI to the skin data, and the bone
remap tables of its meshes after the first (TIKI_GetLocalChannel of each
bone's channel, as RB_SkelMesh does for them). The entry says whether all
of it is there and how many morph weights its vertexes read
===============
*/
static const skinEntry_t *R_SkinTiki( dtiki_t *tiki ) {
	const skinEntry_t *entry = R_SkinFind( tiki, SKIN_TIKI );
	int numMorphTargets = 0, mesh, i;

	if ( entry ) {
		return entry;
	}

	for ( mesh = 0; mesh < tiki->numMeshes; mesh++ ) {
		skelHeaderGame_t *skelmodel = ri.TIKI_GetSkel( tiki->mesh[mesh] );
		skelSurfaceGame_t *sf;

		if ( !skelmodel ) {
			break;
		}
		for ( i = 0, sf = skelmodel->pSurfaces; i < skelmodel->numSurfaces; i++, sf = sf->pNext ) {
			int n = R_SkinAddSurface( sf );

			if ( n < 0 ) {
				break;
			}
			numMorphTargets = Q_max( numMorphTargets, n );
		}
		if ( i < skelmodel->numSurfaces ) {
			break;
		}

		if ( mesh > 0 && !R_SkinFind( tiki, SKIN_REMAP + mesh ) ) {
			uint32_t *remap = ri.Hunk_AllocateTempMemory( sizeof( uint32_t ) * Q_max( skelmodel->numBones, 1 ) );
			int first = 0;

			for ( i = 0; i < skelmodel->numBones; i++ ) {
				int channel = ri.TIKI_GetLocalChannel( tiki, skelmodel->pBones[i].channel );

				if ( channel < 0 ) {
					// RB_SkelMesh would read before the entity's bones
					first = -1;
					break;
				}
				remap[i] = channel;
			}
			if ( !first ) {
				first = vk_add_skin_data( remap, skelmodel->numBones );
			}
			ri.Hunk_FreeTempMemory( remap );
			if ( first < 0 ) {
				break;
			}
			R_SkinInsert( tiki, SKIN_REMAP + mesh, first, 0 );
		}
	}

	return R_SkinInsert( tiki, SKIN_TIKI, mesh == tiki->numMeshes, numMorphTargets );
}

/*
===============
R_SkinGenericStage

A stage of the generic iterator that the skin shader can draw: one texture
with the mesh's texture coordinates or an environment map, no tcMod, and
colors that ComputeColors makes the same for every vertex or that are the
sphere's lighting (an alpha of the stage's own only where the sphere has no
lights, RB_SkinBatchAllowed: the vertex shader's lighting gives alpha 1)
===============
*/
static qboolean R_SkinGenericStage( const shaderStage_t *stage ) {
	const textureBundle_t *bundle = &stage->bundle[0];

	if ( stage->bundle[1].image[0] || stage->multitextureEnv || bundle->numTexMods || bundle->isLightmap
		|| bundle->vertexLightmap ) {
		return qfalse;
	}
	if ( bundle->tcGen != TCGEN_TEXTURE && bundle->tcGen != TCGEN_ENVIRONMENT_MAPPED
		&& bundle->tcGen != TCGEN_ENVIRONMENT_MAPPED2 ) {
		return qfalse;
	}

	switch ( stage->rgbGen ) {
	case CGEN_LIGHTING_GRID:
	case CGEN_LIGHTING_SPHERICAL:
	case CGEN_IDENTITY:
	case CGEN_IDENTITY_LIGHTING:
	case CGEN_CONSTANT:
	case CGEN_ENTITY:
		break;
	default:
		return qfalse;
	}

	switch ( stage->alphaGen ) {
	case AGEN_IDENTITY:
	case AGEN_SKIP:
	case AGEN_ENTITY:
	case AGEN_CONSTANT:
		return qtrue;
	default:
		return qfalse;
	}
}

/*
===============
R_SkinPasses

How many draws per surface a batch of the shader takes when it is skinned
on the GPU: 1 for the vertex-lit iterator, one per stage for the generic
one when the skin shader can draw each of its stages, 0 when the shader is
skinned on the CPU
===============
*/
int R_SkinPasses( const shader_t *shader ) {
	int i;

	if ( shader->optimalStageIteratorFunc == RB_StageIteratorVertexLitTextureUnfogged ) {
		return 1;
	}
	if ( shader->optimalStageIteratorFunc != RB_StageIteratorGeneric || shader->numDeforms || shader->polygonOffset
		|| shader->isSky ) {
		return 0;
	}
	for ( i = 0; i < MAX_SHADER_STAGES && shader->unfoggedStages[i]; i++ ) {
		if ( !R_SkinGenericStage( shader->unfoggedStages[i] ) ) {
			return 0;
		}
	}
	return i;
}

/*
===============
R_SkinRegisterModel

A model registered (R_RegisterModelInternal: cgame registers the models of
the configstrings, the players' and their weapons, as server models): a TIKI
one of whose surfaces has a shader that can be skinned on the GPU
(R_SkinPasses: the batches RB_SkinBatchAllowed can take) gets its skin data
now instead of at its first draw, which made that frame 0.6 ms longer for a
player model.
Only once the map is loaded: its load clears the skin data
(R_ClearSkinData), and R_SkinRegisteredModels adds those registered before
===============
*/
void R_SkinRegisterModel( const model_t *mod ) {
	dtiki_t *tiki;
	int i, j;

	if ( !mod || mod->type != MOD_TIKI || !mod->d.tiki || !tr.world || !r_gpuSkinning->integer
		|| !vk_skin_available() || lod_tool->integer ) {
		return;
	}

	tiki = mod->d.tiki;
	if ( R_SkinFind( tiki, SKIN_TIKI ) ) {
		// added already (cgame registers some models every frame)
		return;
	}
	for ( i = 0; i < tiki->num_surfaces; i++ ) {
		const dtikisurface_t *surface = &tiki->surfaces[i];

		for ( j = 0; j < surface->numskins; j++ ) {
			int h = surface->hShader[j];

			if ( h > 0 && h < tr.numShaders && R_SkinPasses( tr.shaders[h] ) ) {
				R_SkinTiki( tiki );
				return;
			}
		}
	}
}

/*
===============
R_SkinRegisteredModels

At the end of the registration (RE_EndRegistration): the skin data of the
models registered before the map load (in an earlier map too)
===============
*/
void R_SkinRegisteredModels( void ) {
	int i;

	for ( i = 1; i < tr.numModels; i++ ) {
		R_SkinRegisterModel( &tr.models[i] );
	}
}

/*
====================================================================

BATCHES

====================================================================
*/

/*
===============
RB_SkinEntity

The current entity's bones and morph weights in the frame data, once per
frame (its e.bonestart changes with each view it is added to), the view
origin in its space before them
===============
*/
static qboolean RB_SkinEntity( unsigned int serial ) {
	trRefEntity_t *ent = backEnd.currentEntity;
	int numBones, numMorphs, first, prevWords;
	uint32_t *data;

	// in the temporal path's scene, the last frame's bones and morph
	// weights first (VK_SKIN_PREV_WORDS, tr_temporal.c): its shaders read
	// them, so data made without them is made again
	prevWords = backEnd.temporalScene ? VK_SKIN_PREV_WORDS : 0;
	if ( ent->skinSerial == serial && ent->skinBonestart == ent->e.bonestart && ( ent->skinPrev || !prevWords ) ) {
		return qtrue;
	}

	numBones = ri.TIKI_GetNumChannels( ent->e.tiki );
	numMorphs = ent->e.hasMorph ? ent->numMorphs : 0;
	first = vk_alloc_skin_frame( prevWords + 4 + numBones * 16 + numMorphs, &data );
	if ( first < 0 ) {
		return qfalse;
	}
	if ( prevWords ) {
		RB_TemporalSkin( numBones, ent->e.hasMorph, &data[0], &data[1] );
		data[2] = data[3] = 0;
		data += prevWords;
		first += prevWords;
	}
	Com_Memcpy( data, backEnd.ori.viewOrigin, sizeof( vec3_t ) );
	data[3] = 0;
	Com_Memcpy( data + 4, &backEnd.refdef.skelBones[ent->e.bonestart], sizeof( skelBoneCache_t ) * numBones );
	Com_Memcpy( data + 4 + numBones * 16, &backEnd.refdef.skelMorphs[ent->e.morphstart], sizeof( int ) * numMorphs );

	ent->skinSerial = serial;
	ent->skinBonestart = ent->e.bonestart;
	ent->skinPrev = prevWords != 0;
	ent->skinBones = first + 4;
	ent->skinMorphs = ent->e.hasMorph ? first + 4 + numBones * 16 : VK_SKIN_NONE;
	ent->skinLights = VK_SKIN_NONE;
	if ( prevWords ) {
		RB_TemporalRecordSkin( numBones, ent->skinBones, ent->skinMorphs );
	}
	return qtrue;
}

/*
===============
RB_SkinLights

The lights of the current sphere (RB_Light_Real) in the frame data, once
per entity and frame: the entity's sphere is computed once
===============
*/
static qboolean RB_SkinLights( uint32_t *lights ) {
	const sphereor_t *sphere = backEnd.currentSphere;
	trRefEntity_t *ent = backEnd.currentEntity;
	uint32_t *data;
	int first, i;

	if ( ent->skinLights != VK_SKIN_NONE ) {
		*lights = ent->skinLights;
		return qtrue;
	}

	first = vk_alloc_skin_frame( 2 + sphere->numRealLights * 16, &data );
	if ( first < 0 ) {
		return qfalse;
	}
	data[0] = sphere->numRealLights;
	Com_Memcpy( &data[1], sphere->ambient.level, 4 );
	for ( i = 0; i < sphere->numRealLights; i++ ) {
		const reallightinfo_t *light = &sphere->light[i];
		vkSkinLight_t *out = (vkSkinLight_t *)( data + 2 + i * 16 );

		out->type = light->eType;
		VectorCopy( light->color, out->color );
		VectorCopy( light->vOrigin, out->origin );
		VectorCopy( light->vDirection, out->direction );
		out->spotConst = light->fSpotConst;
		out->spotScale = light->fSpotScale;
		out->unused[0] = out->unused[1] = out->unused[2] = out->unused[3] = 0;
	}

	ent->skinLights = *lights = first;
	return qtrue;
}

/*
===============
RB_SkinBatchAllowed

Whether the batch that starts can be skinned on the GPU: its shader takes
the vertex-lit iterator or the generic one with stages the skin shader can
draw (R_SkinPasses), the lighting of the entity's sphere is one the vertex
shader does, and the TIKI and the frame's data fit. Puts the sphere's
lights in the frame data when it has some, and for the vertex-lit iterator
decides the colors: those lights, or the constant the other lighting
functions give (RB_SkinStage does it for each stage of the generic one)
===============
*/
static qboolean RB_SkinBatchAllowed( void ) {
	trRefEntity_t *ent = backEnd.currentEntity;
	void ( *lighting )( unsigned char *colors );
	const skinEntry_t *tiki;
	unsigned int serial;
	uint32_t *data;
	int first, passes, i;

	if ( !r_gpuSkinning->integer || !vk_skin_available() || tess.numVertexes || tess.numIndexes || lod_tool->integer ) {
		return qfalse;
	}
	if ( tess.shader == tr.shadowShader ) {
		return qfalse;
	}
	// debug drawing reads the vertexes
	if ( ( r_showtris->integer || r_shownormals->integer ) && r_developer->integer ) {
		return qfalse;
	}

	if ( tess.currentStageIteratorFunc == RB_StageIteratorVertexLitTextureUnfogged ) {
		// what the iterator calls
		passes = 1;
		lighting = backEnd.currentSphere->TessFunction && r_drawspherelights->integer
			? backEnd.currentSphere->TessFunction : RB_CalcLightGridColor;
	} else if ( tess.currentStageIteratorFunc == RB_StageIteratorGeneric ) {
		// what ComputeColors calls for rgbGen lightingSpherical
		passes = R_SkinPasses( tess.shader );
		if ( !passes ) {
			return qfalse;
		}
		lighting = NULL;
		for ( i = 0; i < passes; i++ ) {
			const shaderStage_t *stage = tess.xstages[i];

			if ( stage->rgbGen == CGEN_LIGHTING_SPHERICAL ) {
				// without either it leaves the colors of an earlier batch
				if ( !r_drawspherelights->integer || !backEnd.currentSphere->TessFunction ) {
					return qfalse;
				}
				lighting = backEnd.currentSphere->TessFunction;
				// the vertex shader's lights give alpha 1, so the stage's
				// own alpha needs the color to be a constant
				if ( ( stage->alphaGen == AGEN_ENTITY || stage->alphaGen == AGEN_CONSTANT ) && lighting == RB_Light_Real
					&& backEnd.currentSphere->numRealLights ) {
					return qfalse;
				}
			}
		}
	} else {
		return qfalse;
	}
	if ( lighting && lighting != RB_Light_Real && lighting != RB_CalcLightGridColor && lighting != RB_Light_Fullbright ) {
		return qfalse;
	}
	if ( lighting == RB_Light_Real && backEnd.currentSphere->bUsesCubeMap ) {
		return qfalse;
	}

	tiki = R_SkinTiki( ent->e.tiki );
	if ( !tiki->value[0] || ( ent->e.hasMorph && ent->numMorphs < tiki->value[1] )
		|| 3 * ent->e.tiki->num_surfaces > (int)ARRAY_LEN( tess.skinSurfs ) ) {
		return qfalse;
	}

	serial = vk_frame_serial();
	if ( !RB_SkinEntity( serial ) ) {
		return qfalse;
	}

	tess.skinSphereLights = VK_SKIN_NONE;
	if ( lighting == RB_Light_Real && backEnd.currentSphere->numRealLights ) {
		if ( !RB_SkinLights( &tess.skinSphereLights ) ) {
			return qfalse;
		}
	}
	if ( tess.currentStageIteratorFunc == RB_StageIteratorVertexLitTextureUnfogged ) {
		tess.skinLights = tess.skinSphereLights;
		if ( tess.skinLights == VK_SKIN_NONE ) {
			// a color that doesn't depend on the vertex
			tess.numVertexes = 1;
			lighting( (unsigned char *)tess.svars.colors );
			tess.numVertexes = 0;
			Com_Memcpy( &tess.skinColor, tess.svars.colors[0], 4 );
		}
	}
	tess.skinEnv = VK_SKIN_ENV_NONE;

	// a draw record for each surface (three times: R_AddSkelSurfaces may
	// add a surface with the same shader up to three times) and pass: the
	// GPU reads them once the whole frame is recorded
	first = vk_alloc_skin_frame( passes * 3 * ent->e.tiki->num_surfaces * 8, &data );
	if ( first < 0 ) {
		return qfalse;
	}
	tess.skinRecords = (vkSkinDraw_t *)data;
	tess.skinFirstRecord = first / 8;
	tess.skinPassesLeft = passes;
	tess.numSkinSurfs = 0;
	return qtrue;
}

/*
===============
RB_SkinSurface

Called by RB_SkelMesh after its level of detail and overflow check: adds
the surface to a skinned batch (deciding at the batch's first surface
whether it is one) and counts the vertexes and indexes RB_SkelMesh would
have added. Returns qfalse when RB_SkelMesh skins it on the CPU
===============
*/
qboolean RB_SkinSurface( skelSurfaceGame_t *sf, int mesh, int render_count ) {
	const skinEntry_t *entry;
	skinBatchSurf_t *s;
	int indexes = sf->numTriangles * 3;

	if ( tess.staticBatch == STATIC_BATCH_UNDECIDED ) {
		tess.staticBatch = RB_SkinBatchAllowed() ? STATIC_BATCH_SKINNED : STATIC_BATCH_OFF;
	}
	if ( tess.staticBatch != STATIC_BATCH_SKINNED ) {
		return qfalse;
	}

	entry = R_SkinFind( sf, SKIN_SURFACE );
	assert( entry && tess.numSkinSurfs < 3 * backEnd.currentEntity->e.tiki->num_surfaces );
	s = &tess.skinSurfs[tess.numSkinSurfs++];
	s->vertex = entry->value[0];
	s->numVertexes = render_count;	// the vertex shader collapses the vertexes from there
	s->remap = mesh ? R_SkinFind( backEnd.currentEntity->e.tiki, SKIN_REMAP + mesh )->value[0] : VK_SKIN_NONE;

	if ( render_count == sf->numVerts ) {
		s->firstIndex = entry->value[1];
		s->numIndexes = indexes;
	} else if ( entry->thresholds ) {
		// RB_SkelMesh's triangles up to the first degenerate one
		const int *thresholds = entry->thresholds;
		int lo = 0, hi = sf->numTriangles;

		while ( lo < hi ) {
			int mid = ( lo + hi ) >> 1;

			if ( thresholds[mid] >= render_count ) {
				hi = mid;
			} else {
				lo = mid + 1;
			}
		}
		s->firstIndex = entry->value[1];
		s->numIndexes = lo * 3;
	} else {
		// RB_SkelMesh's collapsed triangles, relative to the surface's first
		// vertex, up to the first degenerate one
		const skelIndex_t *triangles = sf->pTriangles, *collapse_map = sf->pCollapse;
		short collapse[TIKI_MAX_VERTEXES];
		glIndex_t *out = tess.indexes + tess.numIndexes;
		int i;

		for ( i = 0; i < render_count; i++ ) {
			collapse[i] = i;
		}
		for ( i = render_count; i < sf->numVerts; i++ ) {
			collapse[i] = collapse[collapse_map[i]];
		}
		for ( i = 0; i < indexes; i += 3 ) {
			if ( collapse[triangles[i]] == collapse[triangles[i + 1]] || collapse[triangles[i + 1]] == collapse[triangles[i + 2]]
				|| collapse[triangles[i + 2]] == collapse[triangles[i]] ) {
				break;
			}
			out[i] = collapse[triangles[i]];
			out[i + 1] = collapse[triangles[i + 1]];
			out[i + 2] = collapse[triangles[i + 2]];
		}
		s->firstIndex = -1 - tess.numIndexes;
		s->numIndexes = i;
	}

	tess.numVertexes += render_count;
	tess.numIndexes += s->numIndexes;
	return qtrue;
}

/*
===============
RB_SkinDraw

The draw of the vertex-lit iterator, or of a stage of the generic one, in
a skinned batch: one draw per surface, with its draw record
===============
*/
void RB_SkinDraw( drawGeometry_t *geometry ) {
	trRefEntity_t *ent = backEnd.currentEntity;
	float scale = ent->e.tiki->load_scale * ent->e.scale;
	int i;

	// the batch has draw records for each of its passes
	assert( tess.skinPassesLeft > 0 );
	if ( tess.skinPassesLeft <= 0 ) {
		return;
	}

	geometry->isSkinned = qtrue;
	geometry->isStatic = qfalse;
	for ( i = 0; i < tess.numSkinSurfs; i++ ) {
		const skinBatchSurf_t *s = &tess.skinSurfs[i];
		vkSkinDraw_t *record = &tess.skinRecords[i];

		if ( !s->numIndexes ) {
			continue;
		}
		record->bones = ent->skinBones;
		record->remap = s->remap;
		record->morphs = ent->skinMorphs;
		record->lights = tess.skinLights;
		record->scale = scale;
		record->color = tess.skinColor;
		// indexes computed here are collapsed already (and the collapse map
		// of such a surface may not end)
		record->renderCount = s->firstIndex >= 0 ? s->numVertexes : VK_SKIN_NONE;
		// the skin data has far fewer than 1 << VK_SKIN_ENV_SHIFT vertex records
		record->firstVertex = s->vertex | tess.skinEnv << VK_SKIN_ENV_SHIFT;

		geometry->numVertexes = s->numVertexes;
		geometry->skinVertex = s->vertex;
		geometry->skinDraw = tess.skinFirstRecord + i;
		geometry->skinFirstIndex = s->firstIndex >= 0 ? s->firstIndex : -1;
		vk_draw( PRIM_TRIANGLES, geometry, s->numIndexes, s->firstIndex >= 0 ? NULL : tess.indexes + ( -1 - s->firstIndex ) );
	}

	// the next pass writes its own
	tess.skinRecords += tess.numSkinSurfs;
	tess.skinFirstRecord += tess.numSkinSurfs;
	tess.skinPassesLeft--;
}
