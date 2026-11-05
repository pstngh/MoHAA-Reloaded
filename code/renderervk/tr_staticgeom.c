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

// tr_staticgeom.c -- world surfaces and static models drawn from device memory
//
// The vertexes and indexes of the world's faces and triangle surfaces and of
// the static models don't change once a map is loaded: R_BuildStaticGeometry
// uploads them once (vk_set_static_geometry). A batch of the back end (one
// shader and entity, see RB_RenderDrawSurfList) is then drawn from there,
// instead of copying every vertex into tess and from tess into the vertex
// buffer of the frame, when each stage of its shader takes its colors and
// texture coordinates from the vertexes as they are or as constants
// (RB_StaticBatch, RB_StaticStageColor in tr_shade.c), or the effects shader
// (shaders/effects.vert, r_staticEffects) computes them: the deforms, tcGen
// and tcMod, distFade and the dynamic lights of static models that RB_StaticFx*
// in tr_shade_calc.c accept. Everything else (the other deforms and
// per-vertex rgbGen and alphaGen, the dynamic light pass of r_fastdlights 1,
// the sky, debug drawing, the other surface types) still goes through tess;
// the dynamic lights of r_fastdlights 0 are added per pixel
// (RB_LightmapDlights), static batch or not. The batches and their order
// stay the same: a static batch
// counts the vertexes and indexes it would have put in tess, so it is split
// at the same surfaces, and one that meets a surface it can't draw is
// filled again from its surfaces the usual way (RB_StaticBatchToCPU).
//
// Streams: positions, texture and lightmap coordinates and normals (those
// tess.normal gets: a face's plane normal, the vertexes', a static model's
// pStaticNormal) of the world's surfaces from vertex 0, then of each TIKI
// that static models use, its
// surfaces in the order of R_AddStaticModelSurfaces, so that vertex v of a
// model has color v of its instance's vertex colors (the texture
// coordinates are both streams there). Colors: the world's vertex colors,
// the same with white RGB (rgbGen identity with alphaGen vertex), then the
// static models' (staticModelData). Indexes: those of each world surface,
// grouped by shader in the order a traversal of the BSP tree first reaches
// the surfaces, so that the surfaces of a batch are often one range, drawn
// without copying indexes; then those of each TIKI surface, relative to the
// TIKI's first vertex.

#include "tr_local.h"
#include "../tiki/tiki_shared.h"

typedef struct {
	skelSurfaceGame_t	*surface;
	int					ofsStaticData;	// first vertex in the TIKI's vertexes, staticSurface_t::ofsStaticData
	int					firstIndex;		// -1: no geometry (RB_StaticMesh draws nothing)
} staticTikiSurface_t;

typedef struct staticTiki_s {
	dtiki_t				*tiki;
	int					firstVertex;
	int					numVertexes;
	int					numSurfaces;
	staticTikiSurface_t	*surfaces;
} staticTiki_t;

cvar_t *r_staticGeometry;
cvar_t *r_staticEffects;
cvar_t *r_staticTerrain;
cvar_t *r_terrainCache;

/*
====================================================================

BUILDING

====================================================================
*/

typedef struct {
	int		surface;		// index in tr.world->surfaces
	int		shader;			// shader_t::index
	int		rank;			// order of the first visit of a BSP traversal
} staticOrder_t;

typedef struct {
	staticGeometry_t	g;
	float				*xyz, *st[2], *normals;
	byte				*colors;
	glIndex_t			*indexes;
} staticBuild_t;

/*
===============
R_RankLeafSurfaces

Numbers the world surfaces in the order R_RecursiveWorldNode reaches them
without culling: front child first, the surfaces of a leaf in their order
===============
*/
static void R_RankLeafSurfaces( mnode_t *node, staticOrder_t *order, int *next ) {
	int i;

	while ( node->contents == CONTENTS_NODE ) {
		R_RankLeafSurfaces( node->children[0], order, next );
		node = node->children[1];
	}

	for ( i = 0; i < node->nummarksurfaces; i++ ) {
		staticOrder_t *o = &order[node->firstmarksurface[i] - tr.world->surfaces];

		if ( o->rank < 0 ) {
			o->rank = ( *next )++;
		}
	}
}

static int R_CompareStaticOrder( const void *a, const void *b ) {
	const staticOrder_t *oa = (const staticOrder_t *)a, *ob = (const staticOrder_t *)b;

	if ( oa->shader != ob->shader ) {
		return oa->shader - ob->shader;
	}
	return oa->rank - ob->rank;
}

static void R_AddStaticVertex( staticBuild_t *b, const float *xyz, const float *st, const float *lightmap, const float *normal, const byte *color ) {
	int n = b->g.numVertexes++;

	VectorCopy( xyz, &b->xyz[n * 3] );
	VectorCopy( normal, &b->normals[n * 3] );
	b->st[0][n * 2 + 0] = st[0];
	b->st[0][n * 2 + 1] = st[1];
	b->st[1][n * 2 + 0] = lightmap[0];
	b->st[1][n * 2 + 1] = lightmap[1];
	if ( color ) {
		Com_Memcpy( &b->colors[n * 4], color, 4 );
	}
}

/*
===============
R_AddStaticWorldSurface

The vertexes and indexes of a face or triangle surface; returns its first
index, or -1 when it has none
===============
*/
static int R_AddStaticWorldSurface( staticBuild_t *b, surfaceType_t *data, qboolean *opaque ) {
	int firstVertex = b->g.numVertexes;
	int firstIndex = b->g.numIndexes;
	int i;

	*opaque = qtrue;

	if ( *data == SF_FACE ) {
		srfSurfaceFace_t *face = (srfSurfaceFace_t *)data;
		const int *indexes = (const int *)( (byte *)face + face->ofsIndices );

		for ( i = 0; i < face->numPoints; i++ ) {
			const float *v = face->points[i];

			// RB_SurfaceFace: the plane's normal
			R_AddStaticVertex( b, v, v + 3, v + 5, face->plane.normal, (const byte *)&v[7] );
			if ( ( (const byte *)&v[7] )[3] != 255 ) {
				*opaque = qfalse;
			}
		}
		for ( i = 0; i < face->numIndices; i++ ) {
			b->indexes[b->g.numIndexes++] = firstVertex + indexes[i];
		}
	} else {
		srfTriangles_t *tri = (srfTriangles_t *)data;

		for ( i = 0; i < tri->numVerts; i++ ) {
			const drawVert_t *dv = &tri->verts[i];

			R_AddStaticVertex( b, dv->xyz, dv->st, dv->lightmap, dv->normal, dv->color );
			if ( dv->color[3] != 255 ) {
				*opaque = qfalse;
			}
		}
		for ( i = 0; i < tri->numIndexes; i++ ) {
			b->indexes[b->g.numIndexes++] = firstVertex + tri->indexes[i];
		}
	}

	return b->g.numIndexes > firstIndex ? firstIndex : -1;
}

/*
===============
R_ForEachStaticTikiSurface

Calls func for each surface of a TIKI in the order of
R_AddStaticModelSurfaces, with the surface's first vertex in the model
===============
*/
static int R_ForEachStaticTikiSurface( dtiki_t *tiki, void ( *func )( void *, skelSurfaceGame_t *, int ), void *arg ) {
	int ofsStaticData = 0;
	int mesh, j;

	for ( mesh = 0; mesh < tiki->numMeshes; mesh++ ) {
		skelHeaderGame_t *skelmodel = ri.TIKI_GetSkel( tiki->mesh[mesh] );
		skelSurfaceGame_t *surface;

		if ( !skelmodel ) {
			continue;
		}

		surface = skelmodel->pSurfaces;
		for ( j = 0; j < skelmodel->numSurfaces; j++, ofsStaticData += surface->numVerts, surface = surface->pNext ) {
			func( arg, surface, ofsStaticData );
		}
	}

	return ofsStaticData;
}

typedef struct {
	int		numSurfaces, numIndexes;
} staticTikiCount_t;

static void R_CountStaticTikiSurface( void *arg, skelSurfaceGame_t *surface, int ofsStaticData ) {
	staticTikiCount_t *count = (staticTikiCount_t *)arg;

	count->numSurfaces++;
	if ( surface->pStaticXyz ) {
		count->numIndexes += surface->numTriangles * 3;
	}
}

typedef struct {
	staticBuild_t	*b;
	staticTiki_t	*tiki;
} staticTikiAdd_t;

static void R_AddStaticTikiSurface( void *arg, skelSurfaceGame_t *surface, int ofsStaticData ) {
	staticTikiAdd_t *add = (staticTikiAdd_t *)arg;
	staticBuild_t *b = add->b;
	staticTikiSurface_t *s = &add->tiki->surfaces[add->tiki->numSurfaces++];
	int i;

	s->surface = surface;
	s->ofsStaticData = ofsStaticData;
	s->firstIndex = -1;

	// RB_StaticMesh copies the same arrays
	for ( i = 0; i < surface->numVerts; i++ ) {
		static const float zero[3];

		if ( surface->pStaticXyz ) {
			R_AddStaticVertex( b, surface->pStaticXyz[i], surface->pStaticTexCoords[i][0], surface->pStaticTexCoords[i][1],
				surface->pStaticNormal[i], NULL );
		} else {
			R_AddStaticVertex( b, zero, zero, zero, zero, NULL );
		}
	}

	if ( surface->pStaticXyz ) {
		s->firstIndex = b->g.numIndexes;
		for ( i = 0; i < surface->numTriangles * 3; i++ ) {
			b->indexes[b->g.numIndexes++] = ofsStaticData + surface->pTriangles[i];
		}
	}
}

/*
===============
R_FreeStaticGeometry
===============
*/
void R_FreeStaticGeometry( void ) {
	tr.staticIndexes = NULL;
	tr.numStaticTikis = 0;
	tr.staticTikis = NULL;
	vk_set_static_geometry( NULL );
}

/*
===============
R_BuildStaticGeometry

Called at the end of the map load, once the static models are set up
===============
*/
void R_BuildStaticGeometry( void ) {
	world_t *w = tr.world;
	staticOrder_t *order;
	staticBuild_t b;
	staticTiki_t *tikis;
	glIndex_t *indexes;
	int numWorldVertexes, numVertexes, numIndexes, numColors, numTikis, numTikiSurfaces;
	int i, j, next;

	R_FreeStaticGeometry();

	if ( !w ) {
		return;
	}

	// what the world surfaces need
	numWorldVertexes = numIndexes = 0;
	for ( i = 0; i < w->numsurfaces; i++ ) {
		surfaceType_t *data = w->surfaces[i].data;

		if ( *data == SF_GRID ) {
			numWorldVertexes += ( (srfGridMesh_t *)data )->width * ( (srfGridMesh_t *)data )->height;
		} else if ( *data == SF_FACE ) {
			numWorldVertexes += ( (srfSurfaceFace_t *)data )->numPoints;
			numIndexes += ( (srfSurfaceFace_t *)data )->numIndices;
		} else if ( *data == SF_TRIANGLES ) {
			numWorldVertexes += ( (srfTriangles_t *)data )->numVerts;
			numIndexes += ( (srfTriangles_t *)data )->numIndexes;
		}
	}

	// the TIKIs of the static models, each once
	tikis = w->numStaticModels ? ri.Hunk_Alloc( w->numStaticModels * sizeof( *tikis ), h_dontcare ) : NULL;
	numTikis = numTikiSurfaces = 0;
	numVertexes = numWorldVertexes;
	for ( i = 0; i < w->numStaticModels; i++ ) {
		cStaticModelUnpacked_t *sm = &w->staticModels[i];
		staticTikiCount_t count;

		sm->staticTiki = -1;
		if ( !sm->tiki || !w->staticModelData ) {
			continue;
		}

		for ( j = 0; j < numTikis && tikis[j].tiki != sm->tiki; j++ ) {
		}
		if ( j == numTikis ) {
			Com_Memset( &count, 0, sizeof( count ) );
			tikis[j].tiki = sm->tiki;
			tikis[j].numVertexes = R_ForEachStaticTikiSurface( sm->tiki, R_CountStaticTikiSurface, &count );
			tikis[j].numSurfaces = count.numSurfaces;
			numVertexes += tikis[j].numVertexes;
			numIndexes += count.numIndexes;
			numTikiSurfaces += count.numSurfaces;
			numTikis++;
		}

		// the model's vertex colors (RB_StaticMesh: firstVertexData is a byte
		// offset into staticModelData)
		if ( sm->firstVertexData / 4 + tikis[j].numVertexes <= w->numStaticModelData ) {
			sm->staticTiki = j;
			sm->staticColor = 2 * numWorldVertexes + sm->firstVertexData / 4;
		}
	}

	if ( !numIndexes ) {
		return;
	}

	numColors = 2 * numWorldVertexes + ( w->staticModelData ? w->numStaticModelData : 0 );

	Com_Memset( &b, 0, sizeof( b ) );
	b.xyz = ri.Malloc( numVertexes * sizeof( float ) * 3 );
	b.st[0] = ri.Malloc( numVertexes * sizeof( float ) * 2 );
	b.st[1] = ri.Malloc( numVertexes * sizeof( float ) * 2 );
	b.normals = ri.Malloc( numVertexes * sizeof( float ) * 3 );
	b.colors = ri.Malloc( Q_max( numColors, 1 ) * 4 );
	// kept: batches that are not one range copy their indexes from here
	indexes = ri.Hunk_Alloc( numIndexes * sizeof( glIndex_t ), h_dontcare );
	b.indexes = indexes;

	// world surfaces, grouped by shader in BSP traversal order
	order = ri.Malloc( Q_max( w->numsurfaces, 1 ) * sizeof( *order ) );
	for ( i = 0; i < w->numsurfaces; i++ ) {
		order[i].surface = i;
		order[i].shader = w->surfaces[i].shader ? w->surfaces[i].shader->index : 0;
		order[i].rank = -1;
	}
	next = 0;
	R_RankLeafSurfaces( w->nodes, order, &next );
	for ( i = 0; i < w->numsurfaces; i++ ) {
		if ( order[i].rank < 0 ) {
			// brush models
			order[i].rank = next++;
		}
	}
	qsort( order, w->numsurfaces, sizeof( *order ), R_CompareStaticOrder );

	for ( i = 0; i < w->numsurfaces; i++ ) {
		surfaceType_t *data = w->surfaces[order[i].surface].data;

		if ( *data == SF_FACE ) {
			srfSurfaceFace_t *face = (srfSurfaceFace_t *)data;

			face->staticIndex = R_AddStaticWorldSurface( &b, data, &face->staticOpaque );
		} else if ( *data == SF_TRIANGLES ) {
			srfTriangles_t *tri = (srfTriangles_t *)data;

			tri->staticIndex = R_AddStaticWorldSurface( &b, data, &tri->staticOpaque );
		} else if ( *data == SF_GRID ) {
			// the indexes depend on the level of detail (RB_SurfaceGrid)
			srfGridMesh_t *grid = (srfGridMesh_t *)data;

			grid->staticVertex = b.g.numVertexes;
			grid->staticOpaque = qtrue;
			for ( j = 0; j < grid->width * grid->height; j++ ) {
				const drawVert_t *dv = &grid->verts[j];

				R_AddStaticVertex( &b, dv->xyz, dv->st, dv->lightmap, dv->normal, dv->color );
				if ( dv->color[3] != 255 ) {
					grid->staticOpaque = qfalse;
				}
			}
		}
	}
	ri.Free( order );

	// the same colors with white RGB
	for ( i = 0; i < numWorldVertexes; i++ ) {
		byte *c = &b.colors[( numWorldVertexes + i ) * 4];

		c[0] = c[1] = c[2] = 255;
		c[3] = b.colors[i * 4 + 3];
	}
	if ( w->staticModelData ) {
		Com_Memcpy( &b.colors[2 * numWorldVertexes * 4], w->staticModelData, w->numStaticModelData * 4 );
	}

	// TIKIs
	for ( i = 0; i < numTikis; i++ ) {
		staticTikiAdd_t add;

		tikis[i].firstVertex = b.g.numVertexes;
		tikis[i].surfaces = ri.Hunk_Alloc( Q_max( tikis[i].numSurfaces, 1 ) * sizeof( staticTikiSurface_t ), h_dontcare );
		tikis[i].numSurfaces = 0;
		add.b = &b;
		add.tiki = &tikis[i];
		R_ForEachStaticTikiSurface( tikis[i].tiki, R_AddStaticTikiSurface, &add );
	}

	b.g.xyz = b.xyz;
	b.g.st[0] = b.st[0];
	b.g.st[1] = b.st[1];
	b.g.normals = b.normals;
	b.g.numColors = numColors;
	b.g.colors = b.colors;
	b.g.indexes = b.indexes;
	vk_set_static_geometry( &b.g );

	ri.Free( b.xyz );
	ri.Free( b.st[0] );
	ri.Free( b.st[1] );
	ri.Free( b.normals );
	ri.Free( b.colors );

	tr.staticIndexes = indexes;
	tr.staticWhiteColors = numWorldVertexes;
	tr.staticTikis = tikis;
	tr.numStaticTikis = numTikis;

	ri.Printf( PRINT_DEVELOPER, "static geometry: %d world vertexes, %d TIKIs (%d surfaces) with %d vertexes, %d indexes\n",
		numWorldVertexes, numTikis, numTikiSurfaces, numVertexes - numWorldVertexes, numIndexes );
}

/*
====================================================================

BATCHES

====================================================================
*/

/*
===============
RB_StaticStageAllowed

Whether ComputeColors gives the stage something the static geometry can
draw; notes what that needs (fx: the effects shader computes its colors)
===============
*/
static qboolean RB_StaticStageAllowed( const shaderStage_t *pStage, qboolean *fx ) {
	switch ( RB_StaticStageColor( pStage ) ) {
	case STATIC_COLOR_CPU:
		return qfalse;
	case STATIC_COLOR_VERTEX_OPAQUE:
		tess.staticNeedsOpaque = qtrue;
		break;
	case STATIC_COLOR_CONSTANT:
		// a constant that depends on the model
		if ( pStage->alphaGen == AGEN_TIKI_DIST_FADE || pStage->alphaGen == AGEN_ONE_MINUS_TIKI_DIST_FADE ) {
			tess.staticMergeable = qfalse;
		}
		break;
	default:
		break;
	}
	if ( RB_StaticStageColorFx( pStage ) ) {
		*fx = qtrue;
	}
	return qtrue;
}

/*
===============
RB_StaticBeginInstance

What the following surfaces of the batch belong to: the world (or a brush
model) or the current static model
===============
*/
static void RB_StaticBeginInstance( void ) {
	staticInstance_t *inst = &tess.staticInstances[tess.numStaticInstances++];

	inst->model = backEnd.currentStaticModel;
	Com_Memcpy( inst->modelview, backEnd.ori.modelMatrix, sizeof( inst->modelview ) );
	if ( inst->model ) {
		inst->vertex = tr.staticTikis[inst->model->staticTiki].firstVertex;
		inst->color = inst->model->staticColor;
	} else {
		inst->vertex = 0;
		inst->color = 0;
	}
	inst->firstIndex = 0;
	inst->numIndexes = 0;
	inst->copiedIndex = -1;
}

/*
===============
RB_StaticBatchAllowed

Whether the batch that starts can be drawn from the static geometry: what
its shader's stages do with colors and texture coordinates, which the
entity and cvars decide (dynamic lights are checked per surface)
===============
*/
static qboolean RB_StaticBatchAllowed( void ) {
	shader_t *shader = tess.shader;
	qboolean fx = shader->numDeforms != 0;
	int stage, b, numDraws = 1;

	if ( !r_staticGeometry->integer || tess.numVertexes || tess.numIndexes ) {
		return qfalse;
	}
	// terrain patches draw from the terrain pool, the others from the
	// static geometry
	if ( tess.staticTerrain ? !r_staticTerrain->integer : !tr.staticIndexes ) {
		return qfalse;
	}
	if ( shader->isSky || RB_StaticDlights( tess.dlightBits ) ) {
		return qfalse;
	}
	// the effects shader does the deforms, which only the generic stage
	// iterator makes
	if ( shader->numDeforms && ( tess.currentStageIteratorFunc != RB_StageIteratorGeneric || !RB_StaticFxOn()
		|| !RB_StaticFxDeforms( shader ) ) ) {
		return qfalse;
	}
	// debug drawing reads the vertexes
	if ( ( r_showtris->integer || r_shownormals->integer ) && r_developer->integer ) {
		return qfalse;
	}

	if ( backEnd.currentStaticModel ) {
		if ( backEnd.currentStaticModel->staticTiki < 0 ) {
			return qfalse;
		}
		// as RB_StaticMesh sets it
		tess.vertexColorValid = qtrue;
	}
	tess.staticNeedsOpaque = qfalse;
	// one draw per batch, which a model's stage colors don't depend on
	tess.staticMergeable = backEnd.currentStaticModel && tess.numPasses == 1;

	if ( tess.currentStageIteratorFunc == RB_StageIteratorLightmappedMultitextureUnfogged ) {
		// constant white, texture and lightmap coordinates
	} else if ( tess.currentStageIteratorFunc == RB_StageIteratorVertexLitTextureUnfogged ) {
		// the colors of ComputeColors only for static models
		if ( ( backEnd.currentSphere->TessFunction && r_drawspherelights->integer ) || !backEnd.currentStaticModel
			|| !RB_StaticStageAllowed( tess.xstages[0], &fx ) ) {
			return qfalse;
		}
	} else if ( tess.currentStageIteratorFunc == RB_StageIteratorGeneric ) {
		for ( stage = 0; stage < MAX_SHADER_STAGES && tess.xstages[stage]; stage++ ) {
			shaderStage_t *pStage = tess.xstages[stage];

			if ( !RB_StaticStageAllowed( pStage, &fx ) ) {
				return qfalse;
			}
			for ( b = 0; b < NUM_TEXTURE_BUNDLES && pStage->bundle[b].image[0]; b++ ) {
				switch ( RB_StaticStageSt( &pStage->bundle[b] ) ) {
				case STATIC_ST_NONE:
					return qfalse;
				case STATIC_ST_EFFECTS:
					fx = qtrue;
					break;
				default:
					break;
				}
			}
		}
		numDraws = stage;
	} else {
		return qfalse;
	}

	if ( fx ) {
		// the effects shader reads the static geometry's streams (normals
		// too), which the terrain pool hasn't got
		if ( tess.staticTerrain ) {
			return qfalse;
		}
		// a record per draw (RB_StaticDraw), which the frame data must have
		// room for: the batch is drawn whatever happens
		tess.staticMergeable = qfalse;
		if ( !vk_skin_frame_room( numDraws * STATIC_FX_MAX_WORDS( backEnd.refdef.num_dlights ) ) ) {
			return qfalse;
		}
	}

	// the slots that changed go to the frame's copy of the pool first
	if ( tess.staticTerrain && !RB_TerrainPool() ) {
		return qfalse;
	}

	tess.staticFxBatch = fx;
	tess.numStaticSurfs = 0;
	tess.numStaticInstances = 0;
	tess.staticCopiedIndexes = 0;
	RB_StaticBeginInstance();
	return qtrue;
}

/*
===============
RB_StaticBatch

Whether the current batch is drawn from the static geometry; decided at its
first surface. Called by the functions of the surfaces that can be, after
their overflow check
===============
*/
qboolean RB_StaticBatch( void ) {
	if ( tess.staticBatch == STATIC_BATCH_UNDECIDED ) {
		tess.staticBatch = RB_StaticBatchAllowed() ? STATIC_BATCH_ON : STATIC_BATCH_OFF;
		// a batch of terrain patches that can't is drawn from tess
		tess.staticTerrain = tess.staticTerrain && tess.staticBatch == STATIC_BATCH_ON;
	}
	return tess.staticBatch == STATIC_BATCH_ON;
}

/*
===============
RB_StaticMerge

Whether the next static model, with the same shader, can join the current
batch instead of starting its own (RB_RenderDrawSurfList): when the batch
draws once (one pass, no dynamic light) with stage colors that don't depend
on the model, drawing the models one after the other in one batch gives the
same triangles in the same order as their batches did
===============
*/
qboolean RB_StaticMerge( int staticModel ) {
	return tess.staticBatch == STATIC_BATCH_ON && tess.staticMergeable && !backEnd.refdef.num_dlights
		&& backEnd.refdef.staticModels[staticModel].staticTiki >= 0 && tess.numStaticInstances < MAX_STATIC_INSTANCES;
}

/*
===============
RB_StaticSurface

The surface types whose functions handle the current static batch: a batch
of terrain patches draws from the terrain pool, the others from the static
geometry
===============
*/
qboolean RB_StaticSurface( const surfaceType_t *surface ) {
	if ( tess.staticTerrain ) {
		return *surface == SF_TERRAIN_PATCH;
	}
	return *surface == SF_FACE || *surface == SF_GRID || *surface == SF_TRIANGLES || *surface == SF_TIKI_STATIC;
}

/*
===============
RB_StaticDlights

Whether the dynamic lights of a surface need a pass (ProjectDlightTexture,
r_fastdlights 1: no surface has such bits with 0) in the current batch
===============
*/
qboolean RB_StaticDlights( dlightMask_t dlightBits ) {
	return dlightBits && backEnd.refdef.num_dlights && tess.shader->sort <= SS_OPAQUE;
}

/*
===============
RB_StaticAddSurface

Adds a surface to the static batch unless it needs the CPU: a dynamic light
pass (ProjectDlightTexture runs for the whole batch), or vertex alphas below
255 where a stage takes the vertex colors with alpha 255
===============
*/
qboolean RB_StaticAddSurface( surfaceType_t *surface, dlightMask_t dlightBits, qboolean opaque ) {
	if ( RB_StaticDlights( dlightBits ) || ( !opaque && tess.staticNeedsOpaque ) || tess.numStaticSurfs == ARRAY_LEN( tess.staticSurfs ) ) {
		return qfalse;
	}
	tess.staticSurfs[tess.numStaticSurfs] = surface;
	tess.staticSurfRows[tess.numStaticSurfs] = 0;
	tess.numStaticSurfs++;
	return qtrue;
}

/*
===============
RB_StaticBatchToCPU

The batch can't be drawn from the static geometry after all: its surfaces
are added again the usual way. They fit, since the batch counted the
vertexes and indexes they need. Only world batches get here: the surfaces
of static models always fit (RB_StaticMesh), so merged batches never do
===============
*/
void RB_StaticBatchToCPU( void ) {
	int numSurfs = tess.numStaticSurfs;
	int i;

	assert( tess.numStaticInstances == 1 );
	tess.staticBatch = STATIC_BATCH_OFF;
	tess.staticTerrain = qfalse;
	tess.numVertexes = 0;
	tess.numIndexes = 0;
	tess.dlightBits = 0;
	tess.numStaticSurfs = 0;

	for ( i = 0; i < numSurfs; i++ ) {
		surfaceType_t *surface = tess.staticSurfs[i];

		if ( *surface == SF_GRID ) {
			// the rows of this batch
			RB_SurfaceGridRows( (srfGridMesh_t *)surface, tess.staticSurfRows[i] >> 16, tess.staticSurfRows[i] & 0xFFFF );
		} else {
			rb_surfaceTable[*surface]( surface );
		}
	}
}

/*
===============
RB_StaticCopyIndexes

The current instance is no longer one range of the static index stream: its
indexes go to tess.indexes from now on. Returns where the next ones go
(RB_StaticIndexesAdded)
===============
*/
glIndex_t *RB_StaticCopyIndexes( void ) {
	staticInstance_t *inst = &tess.staticInstances[tess.numStaticInstances - 1];

	if ( inst->copiedIndex < 0 ) {
		inst->copiedIndex = tess.staticCopiedIndexes;
		Com_Memcpy( tess.indexes + tess.staticCopiedIndexes, tr.staticIndexes + inst->firstIndex, sizeof( glIndex_t ) * inst->numIndexes );
		tess.staticCopiedIndexes += inst->numIndexes;
	}
	return tess.indexes + tess.staticCopiedIndexes;
}

/*
===============
RB_StaticIndexesAdded

numIndexes indexes were written where RB_StaticCopyIndexes said
===============
*/
void RB_StaticIndexesAdded( int numIndexes ) {
	tess.staticInstances[tess.numStaticInstances - 1].numIndexes += numIndexes;
	tess.staticCopiedIndexes += numIndexes;
	tess.numIndexes += numIndexes;
}

/*
===============
RB_StaticAddRange

Adds indexes of the static index stream to the current instance
===============
*/
void RB_StaticAddRange( int firstIndex, int numIndexes ) {
	staticInstance_t *inst = &tess.staticInstances[tess.numStaticInstances - 1];

	if ( inst->copiedIndex < 0 ) {
		if ( !inst->numIndexes ) {
			inst->firstIndex = firstIndex;
		} else if ( inst->firstIndex + inst->numIndexes != firstIndex ) {
			Com_Memcpy( RB_StaticCopyIndexes(), tr.staticIndexes + firstIndex, sizeof( glIndex_t ) * numIndexes );
			RB_StaticIndexesAdded( numIndexes );
			return;
		}
		inst->numIndexes += numIndexes;
		tess.numIndexes += numIndexes;
		return;
	}

	Com_Memcpy( tess.indexes + tess.staticCopiedIndexes, tr.staticIndexes + firstIndex, sizeof( glIndex_t ) * numIndexes );
	RB_StaticIndexesAdded( numIndexes );
}

/*
===============
RB_StaticTikiIndex

First index of a surface of the current static model in the static index
stream (-1 for none); starts the model's instance in a merged batch
===============
*/
int RB_StaticTikiIndex( const staticSurface_t *staticSurf ) {
	const staticTiki_t *tiki = &tr.staticTikis[backEnd.currentStaticModel->staticTiki];
	int i;

	if ( tess.staticInstances[tess.numStaticInstances - 1].model != backEnd.currentStaticModel ) {
		RB_StaticBeginInstance();
	}

	for ( i = 0; i < tiki->numSurfaces; i++ ) {
		if ( tiki->surfaces[i].ofsStaticData == staticSurf->ofsStaticData && tiki->surfaces[i].surface == staticSurf->surface ) {
			return tiki->surfaces[i].firstIndex;
		}
	}
	return -1;
}

/*
===============
RB_StaticDraw

A draw of the stage iterators in a static batch: the arrays they set are
those of tess, which stand for the streams of the batch and the stage
(RB_StaticStage); one draw per instance. With deforms, or stage colors or
texture coordinates the effects shader computes, the draw has its record
(and the batch one instance, see RB_StaticBatchAllowed). So do the other
draws of such a batch, the plain stages too: every pass of a surface then
runs the same vertex shader. The later passes test their depth against the
earlier ones' (depthFunc lequal or equal), and two different vertex
shaders needn't round the same positions the same way: with the reflection
(effects) under the texture (generic) of frenchfloor_wood1, a GPU that
rounded them differently drew the reflection over the floor in patches
===============
*/
void RB_StaticDraw( drawGeometry_t *geometry ) {
	const clientArrays_t *arrays = &glState.arrays;
	staticFxUnit_t units[NUM_TEXTURE_BUNDLES];
	const float *modelview;
	qboolean stageColors, fx;
	int colorFx, i;

	geometry->isStatic = qtrue;
	geometry->staticTerrain = tess.staticTerrain;
	geometry->staticFx = -1;
	geometry->staticFxMotion = qfalse;

	stageColors = arrays->colorsEnabled && arrays->colors == tess.svars.colors[0];
	if ( arrays->colorsEnabled && ( !stageColors || tess.staticStageColor == STATIC_COLOR_CONSTANT ) ) {
		// the stage's constant, or tess.constantColor255
		const byte *c = stageColors ? tess.staticStageConstant : tess.constantColor255[0];

		for ( i = 0; i < 4; i++ ) {
			geometry->color[i] = c[i] / 255.0f;
		}
	}

	// what the effects shader does to the stage's colors, the constant too
	colorFx = stageColors ? tess.staticStageFx : 0;
	fx = tess.staticFxBatch || tess.shader->numDeforms || colorFx;

	for ( i = 0; i < NUM_TEXTURE_BUNDLES; i++ ) {
		const float *st = geometry->st[i];

		if ( !st ) {
			geometry->staticSt[i] = STATIC_ST_NONE;
		} else if ( st == tess.texCoords[0][0] ) {
			geometry->staticSt[i] = STATIC_ST_TEXTURE;
		} else if ( st == tess.texCoords[0][1] ) {
			geometry->staticSt[i] = STATIC_ST_LIGHTMAP;
		} else {
			geometry->staticSt[i] = tess.staticStageSt[st == tess.svars.texcoords[0][0] ? 0 : 1];
		}

		switch ( geometry->staticSt[i] ) {
		case STATIC_ST_TEXTURE:
			units[i] = STATIC_FX_UNIT_TEXTURE;
			break;
		case STATIC_ST_LIGHTMAP:
			units[i] = STATIC_FX_UNIT_LIGHTMAP;
			break;
		case STATIC_ST_EFFECTS:
			units[i] = st == tess.svars.texcoords[0][0] ? STATIC_FX_UNIT_BUNDLE0 : STATIC_FX_UNIT_BUNDLE1;
			fx = qtrue;
			break;
		default:
			units[i] = STATIC_FX_UNIT_OFF;
			break;
		}
	}

	if ( fx ) {
		// the colors per vertex of the instance (below), or the constant
		qboolean constant = !stageColors || tess.staticStageColor == STATIC_COLOR_CONSTANT;

		assert( tess.numStaticInstances == 1 );
		geometry->staticFx = RB_StaticFxRecord( tess.staticFxStage, colorFx, units, constant ? geometry->color : NULL, &geometry->staticFxMotion );
		if ( geometry->staticFx < 0 ) {
			// RB_StaticBatchAllowed made sure there is room
			assert( 0 );
			return;
		}
	}

	modelview = glState.modelview;
	for ( i = 0; i < tess.numStaticInstances; i++ ) {
		const staticInstance_t *inst = &tess.staticInstances[i];

		if ( !inst->numIndexes ) {
			continue;
		}

		// the models that joined the batch changed the modelview, also
		// those whose surfaces had nothing to draw at their level of detail
		if ( inst->model && memcmp( modelview, inst->modelview, sizeof( inst->modelview ) ) ) {
			modelview = inst->modelview;
			vk_set_modelview( modelview );
		}

		geometry->staticVertex = inst->vertex;
		geometry->staticColor = -1;
		if ( stageColors && tess.staticTerrain && tess.staticStageColor != STATIC_COLOR_CONSTANT ) {
			// the vertex colors of terrain are white with alpha 255 (all
			// the cases): the pool's colors
			geometry->staticColor = 0;
		} else if ( stageColors && tess.staticStageColor == STATIC_COLOR_WHITE_ALPHA ) {
			geometry->staticColor = tr.staticWhiteColors;
		} else if ( stageColors && tess.staticStageColor != STATIC_COLOR_CONSTANT ) {
			geometry->staticColor = inst->color;
		}

		if ( inst->copiedIndex < 0 ) {
			geometry->staticFirstIndex = inst->firstIndex;
			vk_draw( PRIM_TRIANGLES, geometry, inst->numIndexes, NULL );
		} else {
			geometry->staticFirstIndex = -1;
			vk_draw( PRIM_TRIANGLES, geometry, inst->numIndexes, tess.indexes + inst->copiedIndex );
		}
	}

	if ( modelview != glState.modelview ) {
		vk_set_modelview( glState.modelview );
	}
}
