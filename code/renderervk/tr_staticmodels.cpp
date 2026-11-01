/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

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

// tr_staticmodels.cpp -- static model rendering

#include "tr_local.h"
#include "../corepp/tiki.h"

// MAX_STATIC_MODELS_SURFS: tr_local.h (the frame's pool of them, backEndData_t)
#define MAX_DISTINCT_STATIC_MODELS 1000

qboolean        g_bInfostaticmodels = qfalse;

// The sphere each static model is first culled with, computed once per map
// with the arithmetic of R_AddStaticModelSurfaces: the per-frame loop reads
// this compact array instead of the large cStaticModelUnpacked_t entries
// (the m6l1a forest has 2763 static models, most of them out of view)
typedef struct {
    dtiki_t *tiki; // NULL: not drawn
    vec3_t   origin;
    float    radius;
} staticModelCull_t;

static staticModelCull_t *s_staticModelCull;

/*
==============
R_InitStaticModels
==============
*/
void R_InitStaticModels(void)
{
    cStaticModelUnpacked_t *pSM;
    char                    szTemp[1024];
    skelBoneCache_t         bones[128];
    float                   radius;
    int                     i, j, k, l;

    g_bInfostaticmodels = qfalse;

    s_staticModelCull = NULL;
    if (tr.world->numStaticModels) {
        s_staticModelCull =
            (staticModelCull_t *)ri.Hunk_Alloc(tr.world->numStaticModels * sizeof(staticModelCull_t), h_dontcare);
        Com_Memset(s_staticModelCull, 0, tr.world->numStaticModels * sizeof(staticModelCull_t));
    }

    if (tr.overbrightShift) {
        for (i = 0; i < tr.world->numStaticModelData; i++) {
            int r, g, b;

            r = (int)((float)tr.world->staticModelData[i * 4] * tr.overbrightMult);
            g = (int)((float)tr.world->staticModelData[i * 4 + 1] * tr.overbrightMult);
            b = (int)((float)tr.world->staticModelData[i * 4 + 2] * tr.overbrightMult);

            if (r > 0xFF || g > 0xFF || b > 0xFF) {
                float t;

                t = 255.0 / (float)Q_max(r, Q_max(g, b));

                r = (int)((float)r * t);
                g = (int)((float)g * t);
                b = (int)((float)b * t);
            }

            tr.world->staticModelData[i * 4]     = r;
            tr.world->staticModelData[i * 4 + 1] = g;
            tr.world->staticModelData[i * 4 + 2] = b;
        }
    }

    for (i = 0; i < tr.world->numStaticModels; i++) {
        vec3_t mins, maxs;

        pSM = &tr.world->staticModels[i];

        pSM->bRendered = qfalse;
        AngleVectorsLeft(pSM->angles, pSM->axis[0], pSM->axis[1], pSM->axis[2]);

        if (!strnicmp(pSM->model, "models", 6)) {
            Q_strncpyz(szTemp, pSM->model, sizeof(szTemp));
        } else {
            Com_sprintf(szTemp, sizeof(szTemp), "models/%s", pSM->model);
        }

        ri.FS_CanonicalFilename(szTemp);
        //const bool exists = ri.TIKI_FindTiki(szTemp) != NULL;
        pSM->tiki = ri.TIKI_RegisterTikiFlags(szTemp, qfalse);

        if (!pSM->tiki) {
            ri.Printf(PRINT_WARNING, "^~^~^: Warning: Cannot Load Static Model %s\n", szTemp);
            continue;
        }

        pSM->radius = ri.TIKI_GlobalRadius(pSM->tiki);

        //
        // register shaders
        //
        for (j = 0; j < pSM->tiki->num_surfaces; j++) {
            dtikisurface_t *surf = &pSM->tiki->surfaces[j];

            for (k = 0; k < surf->numskins; k++) {
                if (surf->shader[k][0]) {
                    shader_t *sh = R_FindShader(
                        surf->shader[k], -1, !(surf->flags & TIKI_SURF_NOMIPMAPS), r_picmip->integer, qtrue, qtrue
                    );
                    surf->hShader[k] = sh->index;
                } else {
                    surf->hShader[k] = 0;
                }
            }
        }

        // prepare the skeleton frame for the static model
        ri.TIKI_GetSkelAnimFrame(pSM->tiki, bones, &radius, &mins, &maxs);
        pSM->cull_radius = radius * pSM->tiki->load_scale * pSM->scale;

        {
            staticModelCull_t *cull = &s_staticModelCull[i];
            float              tiki_scale;
            vec3_t             local;

            // R_LocalPointToWorld with the model's orientation
            tiki_scale = pSM->tiki->load_scale * pSM->scale;
            VectorScale(pSM->tiki->load_origin, tiki_scale, local);
            for (k = 0; k < 3; k++) {
                cull->origin[k] = local[0] * pSM->axis[0][k] + local[1] * pSM->axis[1][k] + local[2] * pSM->axis[2][k]
                                + pSM->origin[k];
            }
            cull->radius = pSM->cull_radius;
            cull->tiki   = pSM->tiki;
        }

        // Suggestion:
        // It would be cool to have animated static model in the future

        // Removed in 2.0
        //  This only leads to issues where pStaticXyz is not initialized
        //  and it can occurs if the TIKI model is registered before the static model
        //if (exists) {
        //    continue;
        //}

        for (j = 0; j < pSM->tiki->numMeshes; j++) {
            skelHeaderGame_t  *skelmodel = ri.TIKI_GetSkel(pSM->tiki->mesh[j]);
            skelSurfaceGame_t *surf;

            if (!skelmodel) {
                ri.Printf(PRINT_WARNING, "^~^~^: Warning: Missing mesh in Static Model %s\n", skelmodel->name);
                continue;
            }

            surf = skelmodel->pSurfaces;

            for (k = 0; k < skelmodel->numSurfaces; k++, surf = surf->pNext) {
                byte             *buf;
                byte             *p;
                skelWeight_t     *weight;
                skeletorVertex_t *vert;

                if (surf->pStaticXyz) {
                    continue;
                }

                // allocate static vectors
                p = buf =
                    (byte *)ri.TIKI_Alloc((sizeof(vec4_t) + sizeof(vec4_t) + sizeof(vec2_t) * 2) * surf->numVerts);
                surf->pStaticXyz = (vec4_t *)p;
                p += sizeof(vec4_t) * surf->numVerts;
                surf->pStaticNormal = (vec4_t *)p;
                p += sizeof(vec4_t) * surf->numVerts;
                surf->pStaticTexCoords = (vec2_t(*)[2])p;

                vert = surf->pVerts;

                for (l = 0; l < surf->numVerts; l++) {
                    int              channel;
                    skelBoneCache_t *bone;

                    weight = (skelWeight_t *)((byte *)vert + sizeof(skeletorVertex_t)
                                              + vert->numMorphs * sizeof(skeletorMorph_t));

                    if (j > 0) {
                        channel = ri.TIKI_GetLocalChannel(pSM->tiki, skelmodel->pBones[weight->boneIndex].channel);
                    } else {
                        channel = weight->boneIndex;
                    }

                    bone = &bones[channel];

                    surf->pStaticXyz[l][0] =
                        ((weight->offset[0] * bone->matrix[0][0] + weight->offset[1] * bone->matrix[1][0]
                          + weight->offset[2] * bone->matrix[2][0])
                         + bone->offset[0])
                        * weight->boneWeight;
                    surf->pStaticXyz[l][1] =
                        ((weight->offset[0] * bone->matrix[0][1] + weight->offset[1] * bone->matrix[1][1]
                          + weight->offset[2] * bone->matrix[2][1])
                         + bone->offset[1])
                        * weight->boneWeight;
                    surf->pStaticXyz[l][2] =
                        ((weight->offset[0] * bone->matrix[0][2] + weight->offset[1] * bone->matrix[1][2]
                          + weight->offset[2] * bone->matrix[2][2])
                         + bone->offset[2])
                        * weight->boneWeight;
                    surf->pStaticXyz[l][3] = 0.f;

                    surf->pStaticNormal[l][0] = vert->normal[0] * bone->matrix[0][0]
                                              + vert->normal[1] * bone->matrix[1][0]
                                              + vert->normal[2] * bone->matrix[2][0];
                    surf->pStaticNormal[l][1] = vert->normal[0] * bone->matrix[0][1]
                                              + vert->normal[1] * bone->matrix[1][1]
                                              + vert->normal[2] * bone->matrix[2][1];
                    surf->pStaticNormal[l][2] = vert->normal[0] * bone->matrix[0][2]
                                              + vert->normal[1] * bone->matrix[1][2]
                                              + vert->normal[2] * bone->matrix[2][2];
                    surf->pStaticNormal[l][3] = 0.f;

                    surf->pStaticTexCoords[l][0][0] = vert->texCoords[0];
                    surf->pStaticTexCoords[l][0][1] = vert->texCoords[1];
                    surf->pStaticTexCoords[l][1][0] = vert->texCoords[0];
                    surf->pStaticTexCoords[l][1][1] = vert->texCoords[1];

                    vert = (skeletorVertex_t *)((byte *)vert + sizeof(skeletorVertex_t)
                                                + sizeof(skeletorMorph_t) * vert->numMorphs
                                                + sizeof(skelWeight_t) * vert->numWeights);
                }
            }
        }
    }

    tr.refdef.numStaticModels    = tr.world->numStaticModels;
    tr.refdef.staticModels       = tr.world->staticModels;
    tr.refdef.numStaticModelData = tr.world->numStaticModelData;
    tr.refdef.staticModelData    = tr.world->staticModelData;
}

/*
==============
R_CullStaticModel
==============
*/
static int R_CullStaticModel(dtiki_t *tiki, float fScale, const vec3_t vLocalOrg)
{
    vec3_t bounds[2];
    int    i;
    int    cull;

    for (i = 0; i < 3; i++) {
        bounds[0][i] = vLocalOrg[i] + tiki->a->mins[i] * fScale;
        bounds[1][i] = vLocalOrg[i] + tiki->a->maxs[i] * fScale;
    }

    cull = R_CullLocalBox(bounds);

    if (r_showcull->integer & 4) {
        float  fR, fG, fB;
        vec3_t vAngles;

        switch (cull) {
        case CULL_CLIP:
            fR = 1.0f;
            fG = 1.0f;
            fB = 0.0f;
            break;
        case CULL_IN:
            fR = 0.0f;
            fG = 1.0f;
            fB = 0.0f;
            break;
        case CULL_OUT:
            fR = 1.0f;
            fG = 0.2f;
            fB = 0.2f;
            for (i = 0; i < 2; i++) {
                bounds[0][i] = bounds[0][i] - 16.0;
                bounds[1][i] = bounds[1][i] + 16.0;
            }
            break;
        default:
            break;
        }

        MatrixToEulerAngles(tr.ori.axis, vAngles);
        R_DebugRotatedBBox(tr.ori.origin, vAngles, bounds[0], bounds[1], fR, fG, fB, 0.5);
    }

    switch (cull) {
    case CULL_CLIP:
        tr.pc.c_box_cull_md3_clip++;
        break;
    case CULL_IN:
        tr.pc.c_box_cull_md3_in++;
        break;
    case CULL_OUT:
        tr.pc.c_box_cull_md3_out++;
        break;
    }

    return cull;
}

/*
==============
R_AddStaticModelSurfaces
==============
*/
void R_AddStaticModelSurfaces(void)
{
    cStaticModelUnpacked_t *SM, *lastSM;
    int                     i, j, k;
    int                     ofsStaticData;
    int                     iRadiusCull;
    dtiki_t                *tiki;
    float                   tiki_scale;
    vec3_t                  tiki_localorigin;
    vec3_t                  tiki_worldorigin;

    if (!tr.world->numStaticModels) {
        return;
    }

    tr.shiftedIsStatic = (1 << QSORT_STATICMODEL_SHIFT);
    lastSM             = NULL;

    for (i = 0; i < tr.world->numStaticModels; i++) {
        staticModelCull_t *cull = &s_staticModelCull[i];

        //if( SM->visCount != tr.visCounts[ tr.visIndex ] ) {
        //	continue;
        //}

        if (!cull->tiki) {
            continue;
        }

        SM     = &tr.world->staticModels[i];
        lastSM = SM;

        iRadiusCull = R_CullPointAndRadius(cull->origin, cull->radius);
        if (iRadiusCull == CULL_OUT && !(r_showcull->integer & 8)) {
            continue;
        }

        tiki = SM->tiki;

        tr.currentEntityNum = i;
        tr.shiftedEntityNum = i << QSORT_ENTITYNUM_SHIFT;

        // the culling below only uses the origin and axes; the model matrix
        // of R_RotateForStaticModel is only computed for the last model
        // after the loop (the back end computes its own)
        VectorCopy(SM->origin, tr.ori.origin);
        AxisCopy(SM->axis, tr.ori.axis);

        ofsStaticData = 0;

        // the world position, computed by R_InitStaticModels
        tiki_scale = tiki->load_scale * SM->scale;
        VectorScale(tiki->load_origin, tiki_scale, tiki_localorigin);
        VectorCopy(cull->origin, tiki_worldorigin);

        if (r_showcull->integer & 8) {
            switch (iRadiusCull) {
            case CULL_IN:
                R_DebugCircle(tiki_worldorigin, SM->cull_radius * 1.2, 0.0, 1.0, 0.0, 0.5, 0);
                break;
            case CULL_OUT:
                R_DebugCircle(tiki_worldorigin, SM->cull_radius * 1.4 + 16.0, 1.0, 0.2, 0.2, 0.5, 0);
                break;
            }
        }

        if (iRadiusCull != CULL_OUT
            && (iRadiusCull != CULL_CLIP || R_CullStaticModel(SM->tiki, tiki_scale, tiki_localorigin) != CULL_OUT)) {
            dtikisurface_t *dsurf;

            if (tr.viewParms.isPortal) {
                SM->lodpercentage[1] = R_CalcLod(tiki_worldorigin, SM->cull_radius / SM->scale);
            } else {
                SM->lodpercentage[0] = R_CalcLod(tiki_worldorigin, SM->cull_radius / SM->scale);
            }

            //
            // draw all meshes
            //
            dsurf = tiki->surfaces;
            for (int mesh = 0; mesh < tiki->numMeshes; mesh++) {
                skelHeaderGame_t  *skelmodel = ri.TIKI_GetSkel(tiki->mesh[mesh]);
                skelSurfaceGame_t *surface;
                staticSurface_t   *s_surface;
                shader_t          *shader;
                float              fDist;
                vec3_t             vDelta;

                if (!skelmodel) {
                    continue;
                }

                //
                // draw all surfaces
                //
                surface = skelmodel->pSurfaces;
                for (j = 0; j < skelmodel->numSurfaces;
                     j++, ofsStaticData += surface->numVerts, surface = surface->pNext, dsurf++) {
                    if (backEndData->numStaticSurfaces >= MAX_STATIC_MODELS_SURFS) {
                        ri.Printf(
                            PRINT_DEVELOPER,
                            "^~^~^ ERROR: MAX_STATIC_MODELS_SURFS exceeded - surface of '%s' skipped\n",
                            tiki->a->name
                        );
                        continue;
                    }

                    // the frame's own pool (backEndData_t), with the level of detail the
                    // back end draws it at: the render thread reads it while the next
                    // frame fills its own
                    backEndData->staticSurfaces[backEndData->numStaticSurfaces].lod = SM->lodpercentage[0];
                    s_surface                = &backEndData->staticSurfaces[backEndData->numStaticSurfaces++].surf;
                    s_surface->ident         = SF_TIKI_STATIC;
                    s_surface->ofsStaticData = ofsStaticData;
                    s_surface->surface       = surface;
                    s_surface->meshNum       = mesh;

                    shader = tr.shaders[dsurf->hShader[0]];

                    if (shader->numUnfoggedPasses == 1 && !r_nocull->integer) {
                        switch (shader->unfoggedStages[0]->alphaGen) {
                        case AGEN_DIST_FADE:
                            if (R_DistanceCullPointAndRadius(
                                    shader->fDistNear + shader->fDistRange, tiki_worldorigin, SM->cull_radius
                                )
                                == CULL_OUT) {
                                continue;
                            }
                            break;
                        case AGEN_ONE_MINUS_DIST_FADE:
                            if (R_DistanceCullPointAndRadius(shader->fDistNear, tiki_worldorigin, SM->cull_radius)
                                == CULL_IN) {
                                continue;
                            }
                            break;
                        case AGEN_TIKI_DIST_FADE:
                            fDist = (shader->fDistNear + shader->fDistRange);
                            VectorSubtract(tiki_worldorigin, tr.viewParms.ori.origin, vDelta);
                            if (VectorLengthSquared(vDelta) >= Square(fDist)) {
                                continue;
                            }
                            break;
                        case AGEN_ONE_MINUS_TIKI_DIST_FADE:
                            fDist = (shader->fDistNear + shader->fDistRange);
                            VectorSubtract(tiki_worldorigin, tr.viewParms.ori.origin, vDelta);
                            if (VectorLengthSquared(vDelta) <= Square(shader->fDistNear)) {
                                continue;
                            }
                            break;
                        }
                    }

                    SM->bRendered = qtrue;
                    R_AddDrawSurf((surfaceType_t *)s_surface, shader, 0);

                    if (r_showstaticlod->integer) {
                        vec3_t org;
                        int    render_count, total_tris;

                        VectorCopy(SM->origin, org);
                        org[2] += 100.0;
                        R_DrawDebugNumber(org, SM->lodpercentage[0], r_showstaticlod->value * 2, 1.0, 1.0, 0.0, 3);

                        org[2] += 125.0;
                        R_CountTikiLodTris(tiki, SM->lodpercentage[0], &render_count, &total_tris);
                        R_DrawDebugNumber(org, render_count, r_showstaticlod->value * 2, 1.0, 1.0, 0.0, 0);
                    }

                    if (r_showstaticbboxes->integer) {
                        vec3_t vMins, vMaxs;

                        for (k = 0; k < 3; k++) {
                            vMins[k] = tiki->a->mins[k] * tiki->load_scale * SM->scale;
                            vMaxs[k] = tiki->a->maxs[k] * tiki->load_scale * SM->scale;
                        }

                        R_DebugRotatedBBox(SM->origin, SM->angles, vMins, vMaxs, 1.0, 0.0, 1.0, 0.75);
                    }
                }
            }
        }
    }

    // the state the last model with a TIKI left, culled or not
    if (lastSM) {
        tr.currentEntityNum = lastSM - tr.world->staticModels;
        tr.shiftedEntityNum = tr.currentEntityNum << QSORT_ENTITYNUM_SHIFT;
        R_RotateForStaticModel(lastSM, &tr.viewParms, &tr.ori);
    }

    tr.shiftedIsStatic = 0;
}

/*
==============
RB_Static_BuildDLights
==============
*/
void RB_Static_BuildDLights()
{
    int i;

    backEnd.currentStaticModel->useSpecialLighting = backEnd.refdef.num_dlights > 0;
    backEnd.currentStaticModel->numdlights         = 0;

    if (!backEnd.currentStaticModel->useSpecialLighting) {
        return;
    }

    for (i = 0; i < backEnd.refdef.num_dlights && backEnd.currentStaticModel->numdlights != MAX_SCENE_DLIGHTS; i++) {
        vec3_t lightorigin, delta;

        VectorCopy(backEnd.refdef.dlights[i].origin, lightorigin);
        VectorSubtract(lightorigin, backEnd.currentStaticModel->origin, delta);
        if (backEnd.refdef.dlights[i].radius * 2.0 >= VectorLength(delta)) {
            MatrixTransformVectorRight(
                backEnd.currentStaticModel->axis,
                delta,
                backEnd.staticModelDlights[backEnd.currentStaticModel->numdlights].transformed
            );

            backEnd.staticModelDlights[backEnd.currentStaticModel->numdlights++].index = i;
        }
    }

    if (!backEnd.currentStaticModel->numdlights) {
        backEnd.currentStaticModel->useSpecialLighting = qfalse;
    }
}

/*
==============
R_InfoStaticModels_f
==============
*/
void R_InfoStaticModels_f(void)
{
    g_bInfostaticmodels = qtrue;
}

/*
==============
R_PrintInfoStaticModels
==============
*/
void R_PrintInfoStaticModels()
{
    int                     iRenderCount;
    int                     i, j;
    cStaticModelUnpacked_t *SM;
    dtiki_t                *tikis[MAX_DISTINCT_STATIC_MODELS];
    int                     tiki_count[MAX_DISTINCT_STATIC_MODELS];
    cStaticModelUnpacked_t *pSM[MAX_DISTINCT_STATIC_MODELS];
    int                     count;

    ri.Printf(PRINT_ALL, "Static model info:\n");
    ri.Printf(PRINT_ALL, "------------------\n");

    count        = 0;
    iRenderCount = 0;

    for (i = 0; i < tr.world->numStaticModels; i++) {
        SM = &tr.world->staticModels[i];
        if (!SM->bRendered) {
            continue;
        }

        SM->bRendered = qfalse;
        iRenderCount++;

        if (!SM->tiki) {
            ri.Printf(PRINT_ALL, "ERROR: static model with no tiki\n");
            continue;
        }

        for (j = 0; j < count; j++) {
            if (SM->tiki == tikis[j]) {
                tiki_count[j]++;
                break;
            }
        }

        if (count >= MAX_DISTINCT_STATIC_MODELS) {
            ri.Printf(PRINT_ALL, "R_PrintInfoStaticModels: MAX_DISTINCT_STATIC_MODELS exceeded - increase and recompile\n");
            break;
        }

        if (j == count) {
            tikis[count]      = SM->tiki;
            pSM[count]        = SM;
            tiki_count[count] = 1;

            count++;
        }
    }

    ri.Printf(PRINT_ALL, "Total static models rendered: %d\n", iRenderCount);

    for (i = 0; i < count; i++) {
        skelHeaderGame_t *skelmodel = ri.TIKI_GetSkel(tikis[i]->mesh[0]);

        ri.Printf(PRINT_ALL, 
            "model: %s, version: %d, count: %d,\n culling min %.1f %.1f %.1f, max %.1f %.1f %.1f, radius %.1f\n",
            tikis[i]->a->name,
            skelmodel ? skelmodel->version : -1,
            tiki_count[i],
            tikis[i]->a->mins[0],
            tikis[i]->a->mins[1],
            tikis[i]->a->mins[2],
            tikis[i]->a->maxs[0],
            tikis[i]->a->maxs[1],
            tikis[i]->a->maxs[2],
            pSM[i]->cull_radius
        );
    }
}
