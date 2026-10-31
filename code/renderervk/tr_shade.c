/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_shade.c

#include "tr_local.h"

/*

  THIS ENTIRE FILE IS BACK END

  This file deals with applying shaders to surface data in the tess struct.
*/

/*
==================
R_DrawElements

r_primitives values other than 0 to 3 disable drawing
==================
*/
static void R_DrawElements( int numVertexes, int numIndexes, const glIndex_t *indexes ) {
	int		primitives;

	primitives = r_primitives->integer;

	if ( primitives < 0 || primitives > 3 ) {
		return;
	}

	RB_DrawElements( numVertexes, numIndexes, indexes );
}


/*
=============================================================

SURFACE SHADERS

=============================================================
*/

shaderCommands_t	tess;
static qboolean	setArraysOnce;

static void RB_BindStageTexCoords( const shaderStage_t *pStage, int bundle );

/*
=================
R_BindAnimatedImage

=================
*/
static void R_BindAnimatedImage( textureBundle_t *bundle ) {
	int index;

	if (bundle->numImageAnimations <= 1) {
		R_Bind(bundle->image[0]);
		return;
	}

	// it is necessary to do this messy calc to make sure animations line up
	// exactly with waveforms of the same frequency
	index = myftol((tess.shaderTime + bundle->imageAnimationPhase) * bundle->imageAnimationSpeed * FUNCTABLE_SIZE);
	index >>= FUNCTABLE_SIZE2;

	if (index < 0) {
		index = 0;	// may happen with shader time offsets
	}
	if (!(bundle->flags & BUNDLE_ANIMATE_ONCE)) {
		index %= bundle->numImageAnimations;
	} else if (index >= bundle->numImageAnimations) {
		index = bundle->numImageAnimations - 1;
	}

	R_Bind(bundle->image[index]);
}

/*
================
DrawTris

Draws triangle outlines for debugging
================
*/
static void DrawTris (shaderCommands_t *input, int flags) {
	R_Bind( tr.whiteImage );
	RB_SetColor3f (1,1,1);

	if (flags == 1) {
		R_State(GLS_POLYMODE_LINE);
	} else {
        R_State(GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE);
        vk_set_depth_range(0, 0);
	}

	if (flags < 3) {
		RB_EnableColorArray(qfalse);
		RB_EnableTexCoordArray(qfalse);

		RB_SetVertexArray(input->xyz[0], 16);	// padded for SIMD

		R_DrawElements(input->numVertexes, input->numIndexes, input->indexes);

		vk_set_depth_range(0, 1);
	}

	if (flags > 1) {
		vec3_t start, end;

        start[1] = start[0] = 0.0;
		end[1] = end[0] = 0.0;

        R_State(GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE);
        vk_set_depth_range(0, 0);

        R_EnableTexture(qfalse);
        RB_ImmBegin(IMM_LINES);

        RB_SetColor4f(1.0, 0.0, 0.0, 1.0);

        start[2] = 0.0 - 5.0;
        end[2] = 0.0 + 5.0;
        RB_ImmVertex3fv(start);
        RB_ImmVertex3fv(end);

        RB_SetColor4f(0.0, 1.0, 0.0, 1.0);

        start[2] = start[2] + 5.0;
        end[2] = end[2] - 5.0;
        start[1] = start[1] - 5.0;
        end[1] = end[1] + 5.0;
        RB_ImmVertex3fv(start);
        RB_ImmVertex3fv(end);

        RB_SetColor4f(0.0, 0.0, 1.0, 1.0);

        start[1] = start[1] + 5.0;
        start[0] = start[0] - 5.0;
        end[0] = end[0] + 5.0;
        RB_ImmVertex3fv(start);
        RB_ImmVertex3fv(end);

        start[0] = start[0] + 5.0;
        end[0] = end[0] - 5.0;

        RB_ImmEnd();
        R_EnableTexture(qtrue);
        vk_set_depth_range(0.0, 1.0);

	}
}

/*
================
DrawNormals

Draws vertex normals for debugging
================
*/
static void DrawNormals (shaderCommands_t *input) {
	int		i;
	vec3_t	temp;

	R_Bind( tr.whiteImage );
	RB_SetColor3f (1,1,1);
	vk_set_depth_range( 0, 0 );	// never occluded
	R_State( GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE );

	RB_ImmBegin (IMM_LINES);
	for (i = 0 ; i < input->numVertexes ; i++) {
		RB_ImmVertex3fv (input->xyz[i]);
		VectorMA (input->xyz[i], 2, input->normal[i], temp);
		RB_ImmVertex3fv (temp);
	}
	RB_ImmEnd ();

	vk_set_depth_range( 0, 1 );
}

/*
==============
RB_BeginSurface

We must set some things up before beginning any tesselation,
because a surface may be forced to perform a RB_End due
to overflow.
==============
*/
void RB_BeginSurface( shader_t *shader ) {
	tess.numIndexes = 0;
	tess.numVertexes = 0;
	tess.shader = shader;
	tess.dlightBits = 0;		// will be OR'd in by surface functions
	tess.lightmapDlights = 0;
	tess.lightmapDlightFaces = qfalse;
	tess.vertexColorValid = r_vertexLight->integer != 0;
	tess.xstages = shader->unfoggedStages;
	tess.numPasses = shader->numUnfoggedPasses;
	tess.currentStageIteratorFunc = shader->optimalStageIteratorFunc;
	tess.staticBatch = STATIC_BATCH_UNDECIDED;
	tess.staticTerrain = qfalse;
}

/*
===================
DrawMultitextured

output = t0 * t1 or t0 + t1

t0 = most upstream according to spec
t1 = most downstream according to spec
===================
*/
static void DrawMultitextured( shaderCommands_t *input, int stage, qboolean directTc ) {
	shaderStage_t	*pStage;

	pStage = tess.xstages[stage];

	R_State( pStage->stateBits );

	//
	// base
	//
	R_SelectTexture( 0 );
	if ( directTc ) {
		RB_BindStageTexCoords( pStage, 0 );
	} else {
		RB_SetTexCoordArray( input->svars.texcoords[0][0], 0 );
	}
	R_BindAnimatedImage( &pStage->bundle[0] );

	// MOHAA 1.x configured NV_texture_env_combine4 here for
	// GLS_MULTITEXTURE_ENV stages; removed in 2.0

	//
	// lightmap/secondary pass
	//
	R_SelectTexture( 1 );
	R_EnableTexture( qtrue );
	RB_EnableTexCoordArray( qtrue );

	if ( r_lightmap->integer && pStage->bundle[1].isLightmap ) {
		R_TexEnv( TEXENV_REPLACE );
	} else {
		// MOHAA 1.x had a GLS_MULTITEXTURE_ENV case before this one, using
		// an interpolating combiner; removed in 2.0
		R_TexEnv( pStage->multitextureEnv );
	}

	if ( directTc ) {
		RB_BindStageTexCoords( pStage, 1 );
	} else {
		RB_SetTexCoordArray( input->svars.texcoords[1][0], 0 );
	}

	R_BindAnimatedImage(&pStage->bundle[1]);
	if (pStage->bundle[1].isLightmap) {
		RB_LightmapDlights(1);
	}

	R_DrawElements( input->numVertexes, input->numIndexes, input->indexes );
	RB_LightmapDlights(-1);

	R_EnableTexture( qfalse );
	//
	// disable texturing on TEXTURE1, then select TEXTURE0
	//
	RB_EnableTexCoordArray( qfalse );

	R_SelectTexture( 0 );
}



/*
===================
ProjectDlightTexture

Perform dynamic lighting with another rendering pass
===================
*/
static void ProjectDlightTexture( void ) {
	int		i, l;
#if idppc_altivec
	vec_t	origin0, origin1, origin2;
	float   texCoords0, texCoords1;
	vector float floatColorVec0, floatColorVec1;
	vector float modulateVec, colorVec, zero;
	vector short colorShort;
	vector signed int colorInt;
	vector unsigned char floatColorVecPerm, modulatePerm, colorChar;
	vector unsigned char vSel = (vector unsigned char)(0x00, 0x00, 0x00, 0xff,
							   0x00, 0x00, 0x00, 0xff,
							   0x00, 0x00, 0x00, 0xff,
							   0x00, 0x00, 0x00, 0xff);
#else
	vec3_t	origin;
	vec3_t	normal;
#endif
	float	*texCoords;
	byte	*colors;
	byte	clipBits[SHADER_MAX_VERTEXES];
	MAC_STATIC float	texCoordsArray[SHADER_MAX_VERTEXES][2];
	byte	colorArray[SHADER_MAX_VERTEXES][4];
	unsigned	hitIndexes[SHADER_MAX_INDEXES];
	int		numIndexes;
	int		planetype;
	float	scale;
	float	radius;
	vec3_t	floatColor;
	float	modulate;

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}

#if idppc_altivec
	// There has to be a better way to do this so that floatColor 
	// and/or modulate are already 16-byte aligned.
	floatColorVecPerm = vec_lvsl(0,(float *)floatColor);
	modulatePerm = vec_lvsl(0,(float *)&modulate);
	modulatePerm = (vector unsigned char)vec_splat((vector unsigned int)modulatePerm,0);
	zero = (vector float)vec_splat_s8(0);
#endif

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		dlight_t	*dl;

		if ( !( tess.dlightBits & DLIGHT_BIT( l ) ) ) {
			continue;	// this surface definately doesn't have any of this light
		}
		texCoords = texCoordsArray[0];
		colors = colorArray[0];

		dl = &backEnd.refdef.dlights[l];
#if idppc_altivec
		origin0 = dl->transformed[0];
		origin1 = dl->transformed[1];
		origin2 = dl->transformed[2];
#else
		VectorCopy( dl->transformed, origin );
#endif
		radius = dl->radius * 1.75f;
		scale = 1.75f / radius;

		floatColor[0] = dl->color[0] * 255.0f;
		floatColor[1] = dl->color[1] * 255.0f;
		floatColor[2] = dl->color[2] * 255.0f;
#if idppc_altivec
		floatColorVec0 = vec_ld(0, floatColor);
		floatColorVec1 = vec_ld(11, floatColor);
		floatColorVec0 = vec_perm(floatColorVec0,floatColorVec0,floatColorVecPerm);
#endif
		for ( i = 0 ; i < tess.numVertexes ; i++, texCoords += 2, colors += 4 ) {
#if idppc_altivec
			vec_t dist0, dist1, dist2;
#else
			vec3_t	dist;
#endif
			float	vertdist;
			int		clip;

			backEnd.pc.c_dlightVertexes++;

#if idppc_altivec
			//VectorSubtract( origin, tess.xyz[i], dist );
			dist0 = origin0 - tess.xyz[i][0];
			dist1 = origin1 - tess.xyz[i][1];
			dist2 = origin2 - tess.xyz[i][2];
			texCoords0 = 0.5f + dist0 * scale;
			texCoords1 = 0.5f + dist1 * scale;

			clip = 0;
			if ( texCoords0 < 0.0f ) {
				clip |= 1;
			} else if ( texCoords0 > 1.0f ) {
				clip |= 2;
			}
			if ( texCoords1 < 0.0f ) {
				clip |= 4;
			} else if ( texCoords1 > 1.0f ) {
				clip |= 8;
			}
			texCoords[0] = texCoords0;
			texCoords[1] = texCoords1;
			
			// modulate the strength based on the height and color
			if ( dist2 > radius ) {
				clip |= 16;
				modulate = 0.0f;
			} else if ( dist2 < -radius ) {
				clip |= 32;
				modulate = 0.0f;
			} else {
				dist2 = Q_fabs(dist2);
				if ( dist2 < radius * 0.5f ) {
					modulate = 1.0f;
				} else {
					modulate = 2.0f * (radius - dist2) * scale;
				}
			}
			clipBits[i] = clip;

			modulateVec = vec_ld(0,(float *)&modulate);
			modulateVec = vec_perm(modulateVec,modulateVec,modulatePerm);
			colorVec = vec_madd(floatColorVec0,modulateVec,zero);
			colorInt = vec_cts(colorVec,0);	// RGBx
			colorShort = vec_pack(colorInt,colorInt);		// RGBxRGBx
			colorChar = vec_packsu(colorShort,colorShort);	// RGBxRGBxRGBxRGBx
			colorChar = vec_sel(colorChar,vSel,vSel);		// RGBARGBARGBARGBA replace alpha with 255
			vec_ste((vector unsigned int)colorChar,0,(unsigned int *)colors);	// store color
#else
			VectorCopy(tess.normal[i], normal);

			if (fabs(normal[0]) > fabs(normal[1])) {
				if (fabs(normal[2]) < fabs(normal[0])) {
					planetype = 0;
				} else {
					planetype = 1;
				}
			} else {
				if (fabs(normal[2]) < fabs(normal[1])) {
					planetype = 1;
				}
				else {
					planetype = 2;
				}
			}

			VectorSubtract( origin, tess.xyz[i], dist );
			if (planetype == 0)
			{
				texCoords[0] = 0.5f + dist[1] * scale;
				texCoords[1] = 0.5f + dist[2] * scale;
			}
			else if (planetype == 1)
			{
				texCoords[0] = 0.5f + dist[0] * scale;
				texCoords[1] = 0.5f + dist[2] * scale;
			}
			else
			{
				texCoords[0] = 0.5f + dist[0] * scale;
				texCoords[1] = 0.5f + dist[1] * scale;
			}

			vertdist = DotProduct(dist, normal);
			clip = 0;
			if ( texCoords[0] < 0.0f ) {
				clip |= 1;
			} else if ( texCoords[0] > 1.0f ) {
				clip |= 2;
			}
			if ( texCoords[1] < 0.0f ) {
				clip |= 4;
			} else if ( texCoords[1] > 1.0f ) {
				clip |= 8;
			}
			// modulate the strength based on the height and color
			if (vertdist > radius) {
				clip |= 16;
				modulate = 0.0f;
			}
			else if (vertdist < -radius) {
				clip |= 32;
				modulate = 0.0f;
			}
			else {
				dist[2] = Q_fabs(dist[2]);
				if (dist[2] < radius * 0.5f) {
					modulate = 1.0f;
				}
				else {
					modulate = 2.0f * (radius - dist[2]) * scale;
				}
			}
			clipBits[i] = clip;

			colors[0] = myftol(floatColor[0] * modulate);
			colors[1] = myftol(floatColor[1] * modulate);
			colors[2] = myftol(floatColor[2] * modulate);
			colors[3] = 255;
#endif
		}

		// build a list of triangles that need light
		numIndexes = 0;
		for ( i = 0 ; i < tess.numIndexes ; i += 3 ) {
			int		a, b, c;

			a = tess.indexes[i];
			b = tess.indexes[i+1];
			c = tess.indexes[i+2];
			if ( clipBits[a] & clipBits[b] & clipBits[c] ) {
				continue;	// not lighted
			}
			hitIndexes[numIndexes] = a;
			hitIndexes[numIndexes+1] = b;
			hitIndexes[numIndexes+2] = c;
			numIndexes += 3;
		}

		if ( !numIndexes ) {
			continue;
		}

		RB_EnableColorArray(qtrue);
		RB_EnableTexCoordArray(qtrue);
		RB_SetColorArray(colorArray[0]);
		RB_SetTexCoordArray(texCoordsArray[0], 0);

		R_Bind( tr.dlightImage );
		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		if ( dl->type & additive) {
			R_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		}
		else {
			R_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		}
		R_DrawElements( tess.numVertexes, numIndexes, hitIndexes );
		backEnd.pc.c_totalIndexes += numIndexes;
		backEnd.pc.c_dlightIndexes += numIndexes;
	}
}

/*
===============
ComputeColors
===============
*/
static void ComputeColors( shaderStage_t *pStage )
{
	int		i;

	//
	// rgbGen
	//
	switch ( pStage->rgbGen )
	{
		case CGEN_IDENTITY:
			Com_Memset( tess.svars.colors, 0xff, tess.numVertexes * 4 );
			break;
		default:
		case CGEN_IDENTITY_LIGHTING:
			Com_Memset( tess.svars.colors, tr.identityLightByte, tess.numVertexes * 4 );
			break;
		case CGEN_ENTITY:
			RB_CalcColorFromEntity((unsigned char*)tess.svars.colors);
			break;
		case CGEN_ONE_MINUS_ENTITY:
			RB_CalcColorFromOneMinusEntity((unsigned char*)tess.svars.colors);
			break;
		case CGEN_EXACT_VERTEX:
			if (!tess.vertexColorValid) {
				static qboolean bWarned = qfalse;

				if (!bWarned) {
					// mohta and mohtt shows the warning once
                    bWarned = qtrue;

                    ri.Printf(
                        PRINT_WARNING,
                        "Vertex color specified for shader '%s', but vertex colors are not valid for this model\n",
                        tess.shader->name
					);
				}
				break;
			}
			Com_Memcpy( tess.svars.colors, tess.vertexColors, tess.numVertexes * sizeof( tess.vertexColors[0] ) );
			break;
		case CGEN_VERTEX:
            if (!tess.vertexColorValid) {
                static qboolean bWarned = qfalse;

				if (!bWarned) {
					bWarned = qtrue;

					ri.Printf(
						PRINT_WARNING,
						"Vertex color specified for shader '%s', but vertex colors are not valid for this model\n",
						tess.shader->name);
				}
				break;
			}
			if (tr.identityLight == 1)
			{
				Com_Memcpy(tess.svars.colors, tess.vertexColors, tess.numVertexes * sizeof(tess.vertexColors[0]));
			}
			else
			{
				for (i = 0; i < tess.numVertexes; i++)
				{
					tess.svars.colors[i][0] = tess.vertexColors[i][0] * tr.identityLight;
					tess.svars.colors[i][1] = tess.vertexColors[i][1] * tr.identityLight;
					tess.svars.colors[i][2] = tess.vertexColors[i][2] * tr.identityLight;
					tess.svars.colors[i][3] = tess.vertexColors[i][3];
				}
			}
			break;
		case CGEN_ONE_MINUS_VERTEX:
			if ( tr.identityLight == 1 )
			{
				for ( i = 0; i < tess.numVertexes; i++ )
				{
					tess.svars.colors[i][0] = 255 - tess.vertexColors[i][0];
					tess.svars.colors[i][1] = 255 - tess.vertexColors[i][1];
					tess.svars.colors[i][2] = 255 - tess.vertexColors[i][2];
				}
			}
			else
			{
				for ( i = 0; i < tess.numVertexes; i++ )
				{
					tess.svars.colors[i][0] = ( 255 - tess.vertexColors[i][0] ) * tr.identityLight;
					tess.svars.colors[i][1] = ( 255 - tess.vertexColors[i][1] ) * tr.identityLight;
					tess.svars.colors[i][2] = ( 255 - tess.vertexColors[i][2] ) * tr.identityLight;
				}
			}
			break;
		case CGEN_WAVEFORM:
			RB_CalcWaveColor(&pStage->rgbWave, (unsigned char*)tess.svars.colors, NULL);
			break;
		case CGEN_MULTIPLY_BY_WAVEFORM:
			RB_CalcWaveColor(&pStage->rgbWave, (unsigned char*)tess.svars.colors, pStage->colorConst);
			break;
		case CGEN_LIGHTING_GRID:
			RB_CalcLightGridColor((unsigned char*)tess.svars.colors);
			break;
		case CGEN_LIGHTING_SPHERICAL:
		case CGEN_STATIC:
			if (!r_drawspherelights->integer) {
				break;
			}

			if (!backEnd.currentStaticModel) {
				if (backEnd.currentSphere->TessFunction) {
					backEnd.currentSphere->TessFunction((unsigned char*)tess.svars.colors);
				}
				break;
			}

			if (!tess.vertexColorValid) {
				ri.Printf(PRINT_WARNING, "Vertex color specified for shader '%s', but vertex colors are not valid for this model\n", tess.shader->name);
			}

			if (backEnd.currentStaticModel->useSpecialLighting)
			{
				for (i = 0; i < tess.numVertexes; i++)
				{
					int j;
					vec3_t colorout;
					int r, g, b;

					colorout[0] = tess.vertexColors[i][0];
					colorout[1] = tess.vertexColors[i][1];
					colorout[2] = tess.vertexColors[i][2];

					for (j = 0; j < backEnd.currentStaticModel->numdlights; j++)
					{
						float ooLightDistSquared;
						float dot;
						vec3_t diff;
						dlight_t* dl;

						dl = &backEnd.refdef.dlights[backEnd.staticModelDlights[j].index];
						VectorSubtract(backEnd.staticModelDlights[j].transformed, tess.xyz[i], diff);

						dot = DotProduct(diff, tess.normal[i]);
						if (dot >= 0)
						{
							float ooLen;

							ooLen = 1.0 / VectorLengthSquared(diff);
							ooLightDistSquared = dot * (7500.0 * dl->radius * ooLen * sqrt(ooLen));
							colorout[0] = dl->color[0] * ooLightDistSquared + colorout[0];
							colorout[1] = dl->color[1] * ooLightDistSquared + colorout[1];
							colorout[2] = dl->color[2] * ooLightDistSquared + colorout[2];
						}
					}

					r = colorout[0];
					g = colorout[1];
					b = colorout[2];

					if (tr.overbrightShift)
					{
						r = (int)((float)r * tr.overbrightMult);
						g = (int)((float)g * tr.overbrightMult);
						b = (int)((float)b * tr.overbrightMult);
					}

					if (r > 0xFF || g > 0xFF || b > 0xFF)
					{
						float t;

						t = 255.0 / (float)Q_max(r, Q_max(g, b));

						r = (int)((float)r * t);
						g = (int)((float)g * t);
						b = (int)((float)b * t);
					}

					tess.svars.colors[i][0] = r;
					tess.svars.colors[i][1] = g;
					tess.svars.colors[i][2] = b;
					// Fixed in OPM
					//  This sets the alpha channel which will prevent vertices from hiding
					tess.svars.colors[i][3] = tess.vertexColors[i][3];
				}
			}
			else
			{
				if (tr.identityLight == 1.0)
				{
					Com_Memcpy(tess.svars.colors, tess.vertexColors, tess.numVertexes * sizeof(tess.vertexColors[0]));
				}
				else
				{
					for (i = 0; i < tess.numVertexes; i++) {
						tess.svars.colors[i][0] = tess.vertexColors[i][0] * tr.identityLight;
						tess.svars.colors[i][1] = tess.vertexColors[i][1] * tr.identityLight;
						tess.svars.colors[i][2] = tess.vertexColors[i][2] * tr.identityLight;
					}
				}
			}
			break;
		case CGEN_CONSTANT:
			RB_CalcColorFromConstant((unsigned char*)tess.svars.colors, pStage->colorConst);
			break;
		case CGEN_GLOBAL_COLOR:
			RB_CalcColorFromConstant((unsigned char*)tess.svars.colors, backEnd.color2D);
			break;
		case CGEN_SCOORD:
			RB_CalcRGBFromTexCoords(
				(unsigned char*)tess.svars.colors,
				pStage->alphaMin,
				pStage->alphaMax,
				pStage->alphaConstMin,
				pStage->alphaConst,
				1.0,
				0.0,
				tess.svars.texcoords[0][0]
			);
			break;
		case CGEN_TCOORD:
			RB_CalcRGBFromTexCoords(
				(unsigned char*)tess.svars.colors,
				pStage->alphaMin,
				pStage->alphaMax,
				pStage->alphaConstMin,
				pStage->alphaConst,
				0.0,
				1.0,
				tess.svars.texcoords[0][0]
			);
			break;
		case CGEN_DOT:
			RB_CalcRGBFromDot((unsigned char*)tess.svars.colors, pStage->alphaMin, pStage->alphaMax);
			break;
		case CGEN_ONE_MINUS_DOT:
			RB_CalcRGBFromOneMinusDot((unsigned char*)tess.svars.colors, pStage->alphaMin, pStage->alphaMax);
			break;
	}

	//
	// alphaGen
	//
	switch ( pStage->alphaGen )
	{
	case AGEN_SKIP:
		break;
	case AGEN_IDENTITY:
		if ( pStage->rgbGen != CGEN_IDENTITY
			&& pStage->rgbGen != CGEN_LIGHTING_GRID
			&& pStage->rgbGen != CGEN_LIGHTING_SPHERICAL)
		{
			if ( pStage->rgbGen != CGEN_VERTEX || tr.identityLight != 1 ) {
				for ( i = 0; i < tess.numVertexes; i++ ) {
					tess.svars.colors[i][3] = 0xff;
				}
			}
		}
		break;
	case AGEN_ENTITY:
		RB_CalcAlphaFromEntity((unsigned char*)tess.svars.colors);
		break;
	case AGEN_ONE_MINUS_ENTITY:
		RB_CalcAlphaFromOneMinusEntity((unsigned char*)tess.svars.colors);
		break;
	case AGEN_VERTEX:
		if (pStage->rgbGen != CGEN_VERTEX) {
			for (i = 0; i < tess.numVertexes; i++) {
				tess.svars.colors[i][3] = tess.vertexColors[i][3];
			}
		}
		break;
	case AGEN_ONE_MINUS_VERTEX:
		for (i = 0; i < tess.numVertexes; i++)
		{
			tess.svars.colors[i][3] = 255 - tess.vertexColors[i][3];
		}
		break;
	case AGEN_LIGHTING_SPECULAR:
		RB_CalcSpecularAlpha((unsigned char*)tess.svars.colors, pStage->alphaMax, pStage->specOrigin);
		break;
	case AGEN_WAVEFORM:
		RB_CalcWaveAlpha(&pStage->alphaWave, (unsigned char*)tess.svars.colors);
		break;
	case AGEN_PORTAL:
		{
			unsigned char alpha;

			for ( i = 0; i < tess.numVertexes; i++ )
			{
				float len;
				vec3_t v;

				VectorSubtract( tess.xyz[i], backEnd.viewParms.ori.origin, v );
				len = VectorLength( v );

				len /= tess.shader->portalRange;

				if ( len < 0 )
				{
					alpha = 0;
				}
				else if ( len > 1 )
				{
					alpha = 0xff;
				}
				else
				{
					alpha = len * 0xff;
				}

				tess.svars.colors[i][3] = alpha;
			}
		}
		break;
	case AGEN_DOT:
		RB_CalcAlphaFromDot((unsigned char*)tess.svars.colors, pStage->alphaMin, pStage->alphaMax);
		return;
	case AGEN_ONE_MINUS_DOT:
		RB_CalcAlphaFromOneMinusDot((unsigned char*)tess.svars.colors, pStage->alphaMin, pStage->alphaMax);
		return;
	case AGEN_CONSTANT:
		RB_CalcAlphaFromConstant((unsigned char*)tess.svars.colors, pStage->colorConst[3]);
		break;
	case AGEN_GLOBAL_ALPHA:
		RB_CalcAlphaFromConstant((unsigned char*)tess.svars.colors, backEnd.color2D[3]);
		break;
	case AGEN_SKYALPHA:
		RB_CalcAlphaFromConstant((unsigned char*)tess.svars.colors, backEnd.refdef.sky_alpha * 255.0);
		break;
	case AGEN_ONE_MINUS_SKYALPHA:
		RB_CalcAlphaFromConstant((unsigned char*)tess.svars.colors, (1.0 - backEnd.refdef.sky_alpha) * 255.0);
		break;
	case AGEN_SCOORD:
		RB_CalcAlphaFromTexCoords(
			(unsigned char*)tess.svars.colors,
			pStage->alphaMin,
			pStage->alphaMax,
			pStage->alphaConstMin,
			pStage->alphaConst,
			1.0,
			0.0,
			tess.svars.texcoords[0][0]
		);
		break;
	case AGEN_TCOORD:
		RB_CalcAlphaFromTexCoords(
			(unsigned char*)tess.svars.colors,
			pStage->alphaMin,
			pStage->alphaMax,
			pStage->alphaConstMin,
			pStage->alphaConst,
			0.0,
			1.0,
			tess.svars.texcoords[0][0]
		);
		break;
	case AGEN_DIST_FADE:
		if (backEnd.currentStaticModel)
		{
			unsigned char alpha;
			const float distNear = tess.shader->fDistNear, distRange = tess.shader->fDistRange;
			vec3_t v, eye;

			// the same for every vertex
			VectorSubtract(backEnd.viewParms.ori.origin, backEnd.currentStaticModel->origin, v);
			eye[0] = DotProduct(v, backEnd.currentStaticModel->axis[0]);
			eye[1] = DotProduct(v, backEnd.currentStaticModel->axis[1]);
			eye[2] = DotProduct(v, backEnd.currentStaticModel->axis[2]);

			for (i = 0; i < tess.numVertexes; i++) {
				float len;
				vec3_t org;

				org[0] = tess.xyz[i][0] - eye[0];
				org[1] = tess.xyz[i][1] - eye[1];
				org[2] = tess.xyz[i][2] - eye[2];
			
				len = (VectorLength(org) - distNear) / distRange;
				if (len < 0) {
					alpha = 0xff;
				} else if (len > 1) {
					alpha = 0;
				} else {
					alpha = ((1.0 - len) * 255.0);
                }
                tess.svars.colors[i][3] = alpha;
			}
		}
		else
        {
            unsigned char alpha;

            for (i = 0; i < tess.numVertexes; i++) {
                float len;
                vec3_t org, v;

				if (backEnd.currentEntity) {
					VectorSubtract(backEnd.viewParms.ori.origin, backEnd.currentEntity->e.origin, v);
					org[0] = tess.xyz[i][0] - DotProduct(v, backEnd.currentEntity->e.axis[0]);
					org[1] = tess.xyz[i][1] - DotProduct(v, backEnd.currentEntity->e.axis[1]);
					org[2] = tess.xyz[i][2] - DotProduct(v, backEnd.currentEntity->e.axis[2]);
				} else {
					VectorCopy(backEnd.viewParms.ori.origin, org);
				}

				
				len = (VectorLength(org) - tess.shader->fDistNear) / tess.shader->fDistRange;
				if (len < 0) {
					alpha = 0xff;
				} else if (len > 1) {
					alpha = 0;
				} else {
					alpha = ((1.0 - len) * 255.0);
                }
                tess.svars.colors[i][3] = alpha;
			}
		}
		break;
	case AGEN_ONE_MINUS_DIST_FADE:
		if (backEnd.currentStaticModel)
		{
			unsigned char alpha;
			const float distNear = tess.shader->fDistNear, distRange = tess.shader->fDistRange;
			vec3_t v, eye;

			// the same for every vertex
			VectorSubtract(backEnd.viewParms.ori.origin, backEnd.currentStaticModel->origin, v);
			eye[0] = DotProduct(v, backEnd.currentStaticModel->axis[0]);
			eye[1] = DotProduct(v, backEnd.currentStaticModel->axis[1]);
			eye[2] = DotProduct(v, backEnd.currentStaticModel->axis[2]);

			for (i = 0; i < tess.numVertexes; i++) {
				float len;
				vec3_t org;

				org[0] = tess.xyz[i][0] - eye[0];
				org[1] = tess.xyz[i][1] - eye[1];
				org[2] = tess.xyz[i][2] - eye[2];
			
				len = (VectorLength(org) - distNear) / distRange;
				if (len < 0) {
					alpha = 0;
				} else if (len > 1) {
					alpha = 0xff;
				} else {
					alpha = (len * 255.0);
                }
                tess.svars.colors[i][3] = alpha;
			}
		}
		else
        {
            unsigned char alpha;

            for (i = 0; i < tess.numVertexes; i++) {
                float len;
                vec3_t org, v;

				if (backEnd.currentEntity) {
                    VectorSubtract(backEnd.viewParms.ori.origin, backEnd.currentEntity->e.origin, v);
                    org[0] = tess.xyz[i][0] - DotProduct(v, backEnd.currentEntity->e.axis[0]);
                    org[1] = tess.xyz[i][1] - DotProduct(v, backEnd.currentEntity->e.axis[1]);
                    org[2] = tess.xyz[i][2] - DotProduct(v, backEnd.currentEntity->e.axis[2]);
				} else {
					VectorCopy(backEnd.viewParms.ori.origin, org);
				}

				
				len = (VectorLength(org) - tess.shader->fDistNear) / tess.shader->fDistRange;
				if (len < 0) {
					alpha = 0;
				} else if (len > 1) {
					alpha = 0xff;
				} else {
					alpha = (len * 255.0);
                }
                tess.svars.colors[i][3] = alpha;
			}
		}
		break;
    case AGEN_TIKI_DIST_FADE:
    case AGEN_ONE_MINUS_TIKI_DIST_FADE:
		{
			unsigned char alpha;

			if (backEnd.currentStaticModel)
			{
				float lenSqr;
				float fDistNearSqr;
				float fDistFarSqr;
				vec3_t org;

				VectorSubtract(backEnd.currentStaticModel->origin, backEnd.viewParms.ori.origin, org);

				lenSqr = VectorLengthSquared(org);
                fDistNearSqr = tess.shader->fDistNear * tess.shader->fDistNear;
                fDistFarSqr = (tess.shader->fDistNear + tess.shader->fDistRange) * (tess.shader->fDistNear + tess.shader->fDistRange);
				if (lenSqr <= fDistNearSqr) {
					alpha = 0;
				} else if (lenSqr >= fDistFarSqr) {
					alpha = 0xff;
				} else {
					float len;

					len = VectorLength(org);
					alpha = (((VectorLength(org) - tess.shader->fDistNear) / tess.shader->fDistRange) * 255.0);
				}

				if (pStage->alphaGen == AGEN_TIKI_DIST_FADE) {
					alpha = 0xff - alpha;
				}
			}
			else if (backEnd.currentEntity)
			{
				float lenSqr;
				float fDistNearSqr;
				float fDistFarSqr;
				vec3_t org;

				VectorSubtract(backEnd.currentEntity->e.origin, backEnd.viewParms.ori.origin, org);

				lenSqr = VectorLengthSquared(org);
				fDistNearSqr = tess.shader->fDistNear * tess.shader->fDistNear;
				fDistFarSqr = (tess.shader->fDistNear + tess.shader->fDistRange) * (tess.shader->fDistNear + tess.shader->fDistRange);
				if (lenSqr <= fDistNearSqr) {
					alpha = 0;
				} else if (lenSqr >= fDistFarSqr) {
					alpha = 0xff;
				} else {
					float len;

                    len = VectorLength(org);
                    alpha = (((VectorLength(org) - tess.shader->fDistNear) / tess.shader->fDistRange) * 255.0);
				}

				if (pStage->alphaGen == AGEN_TIKI_DIST_FADE) {
					alpha = 0xff - alpha;
				}
			}
			else
			{
				const char* type;
				if (pStage->alphaGen == AGEN_TIKI_DIST_FADE) {
					type = "tikiDistFade";
				} else {
					type = "oneMinusTikiDistFade";
				}

				ri.Error(ERR_DROP, "ERROR: '%s' shader command used on a non-tiki\n", type);
				alpha = 0xff;
			}
			RB_CalcAlphaFromConstant((unsigned char*)tess.svars.colors, alpha);
		}
		break;
	case AGEN_DOT_VIEW:
		RB_CalcAlphaFromDotView((unsigned char*)tess.svars.colors, pStage->alphaMin, pStage->alphaMax);
		break;
	case AGEN_ONE_MINUS_DOT_VIEW:
		RB_CalcAlphaFromOneMinusDotView((unsigned char*)tess.svars.colors, pStage->alphaMin, pStage->alphaMax);
        break;
    case AGEN_HEIGHT_FADE:
        RB_CalcAlphaFromHeightFade((unsigned char*)tess.svars.colors, pStage->alphaMin, pStage->alphaMax);
        break;
	default:
		break;
	}
}

/*
===============
RB_StageConstantColor

Whether a stage drawn on the CPU has the same color at every vertex:
rgbGen identity or identityLighting with alphaGen identity. It is drawn
with that color as the constant (RB_SetStageConstantColor) instead of an
array ComputeColors fills; tr_pipelines.c predicts the same keys
===============
*/
qboolean RB_StageConstantColor( const shaderStage_t *pStage ) {
	return ( pStage->rgbGen == CGEN_IDENTITY || pStage->rgbGen == CGEN_IDENTITY_LIGHTING )
		&& pStage->alphaGen == AGEN_IDENTITY;
}

/*
===============
RB_SetStageConstantColor

ComputeColors' color for such a stage, as the vertexes would have it (bytes)
===============
*/
static void RB_SetStageConstantColor( const shaderStage_t *pStage ) {
	const float c = ( pStage->rgbGen == CGEN_IDENTITY ? 255 : tr.identityLightByte ) / 255.0f;

	RB_EnableColorArray( qfalse );
	RB_SetColor3f( c, c, c );
}

/*
===============
RB_StageTexCoordsDirect

Whether a stage drawn on the CPU can take its texture coordinates from
tess.texCoords (RB_BindStageTexCoords) instead of a copy ComputeTexCoords
makes: every bundle has the mesh's or the lightmap's, without texMods, and
neither rgbGen nor alphaGen reads the copy (sCoord, tCoord)
===============
*/
static qboolean RB_StageTexCoordsDirect( const shaderStage_t *pStage ) {
	int b;

	if ( pStage->rgbGen == CGEN_SCOORD || pStage->rgbGen == CGEN_TCOORD
		|| pStage->alphaGen == AGEN_SCOORD || pStage->alphaGen == AGEN_TCOORD ) {
		return qfalse;
	}
	for ( b = 0; b < NUM_TEXTURE_BUNDLES && pStage->bundle[b].image[0]; b++ ) {
		if ( pStage->bundle[b].numTexMods
			|| ( pStage->bundle[b].tcGen != TCGEN_TEXTURE && pStage->bundle[b].tcGen != TCGEN_LIGHTMAP ) ) {
			return qfalse;
		}
	}
	return pStage->bundle[0].image[0] != NULL;
}

/*
===============
RB_BindStageTexCoords
===============
*/
static void RB_BindStageTexCoords( const shaderStage_t *pStage, int bundle ) {
	RB_SetTexCoordArray( tess.texCoords[0][pStage->bundle[bundle].tcGen == TCGEN_LIGHTMAP], 16 );
}

/*
===============
ComputeTexCoords
===============
*/
static void ComputeTexCoords( shaderStage_t *pStage ) {
	int		i;
	int		b;

	for ( b = 0; b < NUM_TEXTURE_BUNDLES && pStage->bundle[b].image[0]; b++ ) {
		int tm;

		//
		// generate the texture coordinates
		//
		switch ( pStage->bundle[b].tcGen )
		{
		case TCGEN_IDENTITY:
			Com_Memset( tess.svars.texcoords[b], 0, sizeof( float ) * 2 * tess.numVertexes );
			break;
		case TCGEN_TEXTURE:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = tess.texCoords[i][0][0];
				tess.svars.texcoords[b][i][1] = tess.texCoords[i][0][1];
			}
			break;
		case TCGEN_LIGHTMAP:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = tess.texCoords[i][1][0];
				tess.svars.texcoords[b][i][1] = tess.texCoords[i][1][1];
			}
			break;
		case TCGEN_ENVIRONMENT_MAPPED:
			RB_CalcEnvironmentTexCoords( ( float * ) tess.svars.texcoords[b] );
            break;
		case TCGEN_VECTOR:
			for (i = 0; i < tess.numVertexes; i++) {
				tess.svars.texcoords[b][i][0] = DotProduct(tess.xyz[i], pStage->bundle[b].tcGenVectors[0]);
				tess.svars.texcoords[b][i][1] = DotProduct(tess.xyz[i], pStage->bundle[b].tcGenVectors[1]);
			}
			break;
        case TCGEN_ENVIRONMENT_MAPPED2:
            RB_CalcEnvironmentTexCoords2((float*)tess.svars.texcoords[b]);
            break;
		case TCGEN_SUN_REFLECTION:
			RB_CalcSunReflectionTexCoords(tess.svars.texcoords[b][0]);
			break;
        default:
            ri.Printf(
				PRINT_DEVELOPER,
                "WARNING: invalid tcGen '%d' specified for shader '%s'\n",
                pStage->bundle[b].tcGen,
                tess.shader->name
			);
			break;
		}

		//
		// alter texture coordinates
		//
		for ( tm = 0; tm < pStage->bundle[b].numTexMods ; tm++ ) {
			switch ( pStage->bundle[b].texMods[tm].type )
			{
			case TMOD_NONE:
				tm = TR_MAX_TEXMODS;		// break out of for loop
				break;

			case TMOD_TURBULENT:
				RB_CalcTurbulentTexCoords( &pStage->bundle[b].texMods[tm].wave, 
						                 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_ENTITY_TRANSLATE:
				RB_CalcScrollTexCoords( backEnd.currentEntity->e.shaderTexCoord,
									 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_SCROLL:
				RB_CalcScrollTexCoords( pStage->bundle[b].texMods[tm].scroll,
										 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_SCALE:
				RB_CalcScaleTexCoords( pStage->bundle[b].texMods[tm].scale,
									 ( float * ) tess.svars.texcoords[b] );
				break;
			
			case TMOD_STRETCH:
				RB_CalcStretchTexCoords( &pStage->bundle[b].texMods[tm].wave, 
						               ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_TRANSFORM:
				RB_CalcTransformTexCoords( &pStage->bundle[b].texMods[tm],
						                 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_ROTATE:
				RB_CalcRotateTexCoords( pStage->bundle[b].texMods[tm].rotateSpeed,
										pStage->bundle[b].texMods[tm].rotateCoef,
										( float * ) tess.svars.texcoords[b],
										pStage->bundle[b].texMods[tm].rotateStart );
				break;

			case TMOD_OFFSET:
				RB_CalcOffsetTexCoords(pStage->bundle[b].texMods[tm].scroll, tess.svars.texcoords[b][0]);
				break;

            case TMOD_PARALLAX:
				RB_CalcParallaxTexCoords(pStage->bundle[b].texMods[tm].rate, tess.svars.texcoords[b][0]);
                break;

            case TMOD_MACRO:
				RB_CalcMacroTexCoords(pStage->bundle[b].texMods[tm].scale, tess.svars.texcoords[b][0]);
                break;

            case TMOD_WAVETRANS:
				RB_CalcTransWaveTexCoords(&pStage->bundle[b].texMods[tm].wave, tess.svars.texcoords[b][0]);
                break;

            case TMOD_WAVETRANT:
				RB_CalcTransWaveTexCoordsT(&pStage->bundle[b].texMods[tm].wave, tess.svars.texcoords[b][0]);
                break;

            case TMOD_BULGETRANS:
				RB_CalcBulgeTexCoords(&pStage->bundle[b].texMods[tm].wave, tess.svars.texcoords[b][0]);
                break;

			default:
				ri.Printf(PRINT_WARNING, "WARNING: invalid tcMod '%d' specified for shader '%s'\n", pStage->bundle[b].texMods[tm].type, tess.shader->name );
				break;
			}
		}
	}
}

/*
===============
RB_StaticStageColor

What ComputeColors gives the stage in the current static batch (see
tr_staticgeom.c): the same color for every vertex (also for static models'
vertex alpha, always 255), the vertex colors as they are, or with alpha 255
(when the batch's vertex alphas are 255), white with the vertex alpha, or
something only the CPU path gives (per-vertex computations, colors left
from an earlier batch). The vertex colors and the constant also stand for
what the effects shader computes from them (RB_StaticStageColorFx: the
lighting of a static model from its vertex colors, the alpha of distFade)
===============
*/
staticColor_t RB_StaticStageColor( const shaderStage_t *pStage ) {
	staticColor_t rgb, alpha;
	int fx = RB_StaticStageColorFx( pStage );

	// what rgbGen writes, all four channels
	switch ( pStage->rgbGen ) {
	case CGEN_ENTITY:
	case CGEN_ONE_MINUS_ENTITY:
		if ( !backEnd.currentEntity ) {
			return STATIC_COLOR_CPU;
		}
		rgb = STATIC_COLOR_CONSTANT;
		break;
	case CGEN_EXACT_VERTEX:
		if ( !tess.vertexColorValid ) {
			return STATIC_COLOR_CPU;
		}
		rgb = STATIC_COLOR_VERTEX;
		break;
	case CGEN_VERTEX:
		if ( !tess.vertexColorValid || tr.identityLight != 1 ) {
			return STATIC_COLOR_CPU;
		}
		rgb = STATIC_COLOR_VERTEX;
		break;
	case CGEN_LIGHTING_SPHERICAL:
	case CGEN_STATIC:
		if ( fx & STATIC_FX_LIGHTING ) {
			rgb = STATIC_COLOR_VERTEX;
			break;
		}
		if ( !r_drawspherelights->integer || !backEnd.currentStaticModel || backEnd.currentStaticModel->useSpecialLighting
			|| tr.identityLight != 1.0 ) {
			return STATIC_COLOR_CPU;
		}
		rgb = STATIC_COLOR_VERTEX;
		break;
	case CGEN_ONE_MINUS_VERTEX:
	case CGEN_LIGHTING_GRID:
	case CGEN_SCOORD:
	case CGEN_TCOORD:
	case CGEN_DOT:
	case CGEN_ONE_MINUS_DOT:
		return STATIC_COLOR_CPU;
	default:
		rgb = STATIC_COLOR_CONSTANT;
		break;
	}

	switch ( pStage->alphaGen ) {
	case AGEN_IDENTITY:
		if ( pStage->rgbGen != CGEN_IDENTITY && pStage->rgbGen != CGEN_LIGHTING_GRID && pStage->rgbGen != CGEN_LIGHTING_SPHERICAL
			&& ( pStage->rgbGen != CGEN_VERTEX || tr.identityLight != 1 ) ) {
			alpha = STATIC_COLOR_CONSTANT;
		} else {
			alpha = rgb;
		}
		break;
	case AGEN_ENTITY:
	case AGEN_ONE_MINUS_ENTITY:
		alpha = backEnd.currentEntity ? STATIC_COLOR_CONSTANT : rgb;
		break;
	case AGEN_VERTEX:
		alpha = pStage->rgbGen != CGEN_VERTEX ? STATIC_COLOR_VERTEX : rgb;
		break;
	case AGEN_WAVEFORM:
	case AGEN_CONSTANT:
	case AGEN_GLOBAL_ALPHA:
	case AGEN_SKYALPHA:
	case AGEN_ONE_MINUS_SKYALPHA:
		alpha = STATIC_COLOR_CONSTANT;
		break;
	case AGEN_TIKI_DIST_FADE:
	case AGEN_ONE_MINUS_TIKI_DIST_FADE:
		if ( !backEnd.currentStaticModel && !backEnd.currentEntity ) {
			return STATIC_COLOR_CPU;
		}
		alpha = STATIC_COLOR_CONSTANT;
		break;
	case AGEN_DIST_FADE:
	case AGEN_ONE_MINUS_DIST_FADE:
		// the effects shader replaces the alpha of rgbGen's colors
		if ( !( fx & ( 3 << STATIC_FX_ALPHA_SHIFT ) ) ) {
			return STATIC_COLOR_CPU;
		}
		alpha = rgb;
		break;
	case AGEN_ONE_MINUS_VERTEX:
	case AGEN_LIGHTING_SPECULAR:
	case AGEN_PORTAL:
	case AGEN_DOT:
	case AGEN_ONE_MINUS_DOT:
	case AGEN_SCOORD:
	case AGEN_TCOORD:
	case AGEN_DOT_VIEW:
	case AGEN_ONE_MINUS_DOT_VIEW:
	case AGEN_HEIGHT_FADE:
		return STATIC_COLOR_CPU;
	default:
		// AGEN_SKIP: what rgbGen wrote
		alpha = rgb;
		break;
	}

	if ( alpha == rgb ) {
		return rgb;
	}
	if ( rgb == STATIC_COLOR_CONSTANT ) {
		// the vertex alpha
		if ( backEnd.currentStaticModel ) {
			return STATIC_COLOR_CONSTANT;
		}
		if ( pStage->rgbGen == CGEN_IDENTITY || ( pStage->rgbGen == CGEN_IDENTITY_LIGHTING && tr.identityLightByte == 255 ) ) {
			return STATIC_COLOR_WHITE_ALPHA;
		}
		return STATIC_COLOR_CPU;
	}
	// the vertex colors with a constant alpha: alpha 255
	if ( pStage->alphaGen == AGEN_IDENTITY || ( pStage->alphaGen == AGEN_CONSTANT && pStage->colorConst[3] == 255 ) ) {
		return STATIC_COLOR_VERTEX_OPAQUE;
	}
	return STATIC_COLOR_CPU;
}

/*
===============
RB_StaticStageColorFx

What the effects shader computes of ComputeColors for the stage in the
current static batch (STATIC_FX_*): the useSpecialLighting loop of a static
model (its vertex colors and the dynamic lights near it), the alpha of
distFade and oneMinusDistFade
===============
*/
int RB_StaticStageColorFx( const shaderStage_t *pStage ) {
	int fx = 0;

	if ( !RB_StaticFxOn() ) {
		return 0;
	}
	if ( ( pStage->rgbGen == CGEN_LIGHTING_SPHERICAL || pStage->rgbGen == CGEN_STATIC ) && r_drawspherelights->integer
		&& backEnd.currentStaticModel && backEnd.currentStaticModel->useSpecialLighting ) {
		fx |= STATIC_FX_LIGHTING;
	}
	// without either the alpha is the same for every vertex, left to the CPU
	if ( backEnd.currentStaticModel || backEnd.currentEntity ) {
		if ( pStage->alphaGen == AGEN_DIST_FADE ) {
			fx |= 1 << STATIC_FX_ALPHA_SHIFT;
		} else if ( pStage->alphaGen == AGEN_ONE_MINUS_DIST_FADE ) {
			fx |= 2 << STATIC_FX_ALPHA_SHIFT;
		}
	}
	return fx;
}

/*
===============
RB_StaticStageSt

The static stream ComputeTexCoords gives a bundle, STATIC_ST_EFFECTS when
the effects shader computes them, STATIC_ST_NONE when only the CPU does
===============
*/
staticSt_t RB_StaticStageSt( const textureBundle_t *bundle ) {
	if ( bundle->numTexMods && bundle->texMods[0].type != TMOD_NONE ) {
		return RB_StaticFxOn() && RB_StaticFxBundle( bundle, backEnd.currentEntity != NULL ) ? STATIC_ST_EFFECTS : STATIC_ST_NONE;
	}
	if ( bundle->tcGen == TCGEN_TEXTURE ) {
		return STATIC_ST_TEXTURE;
	}
	if ( bundle->tcGen == TCGEN_LIGHTMAP ) {
		return STATIC_ST_LIGHTMAP;
	}
	return RB_StaticFxOn() && RB_StaticFxBundle( bundle, backEnd.currentEntity != NULL ) ? STATIC_ST_EFFECTS : STATIC_ST_NONE;
}

/*
===============
RB_StaticStage

ComputeTexCoords and ComputeColors for a static batch: the streams of the
stage (RB_StaticDraw), a constant evaluated by ComputeColors for one
vertex
===============
*/
void RB_StaticStage( shaderStage_t *pStage ) {
	int b;

	for ( b = 0; b < NUM_TEXTURE_BUNDLES; b++ ) {
		tess.staticStageSt[b] = pStage->bundle[b].image[0] ? RB_StaticStageSt( &pStage->bundle[b] ) : STATIC_ST_NONE;
	}

	tess.staticStageColor = RB_StaticStageColor( pStage );
	tess.staticStageFx = RB_StaticStageColorFx( pStage );
	tess.staticFxStage = pStage;
	if ( tess.staticStageColor == STATIC_COLOR_CONSTANT ) {
		int numVertexes = tess.numVertexes;

		// static models' vertex alpha
		tess.vertexColors[0][3] = 255;
		tess.numVertexes = 1;
		ComputeColors( pStage );
		tess.numVertexes = numVertexes;
		Com_Memcpy( tess.staticStageConstant, tess.svars.colors[0], sizeof( tess.staticStageConstant ) );
	}
}

/*
===============
RB_SkinStage

ComputeTexCoords and ComputeColors for a stage of the generic iterator in a
skinned batch (R_SkinPasses): the vertex shader's texture coordinates, and
the sphere's lights (RB_SkinBatchAllowed) or a constant evaluated by
ComputeColors for one vertex
===============
*/
void RB_SkinStage( shaderStage_t *pStage ) {
	if ( pStage->bundle[0].tcGen == TCGEN_ENVIRONMENT_MAPPED ) {
		tess.skinEnv = VK_SKIN_ENV_MAPPED;
	} else if ( pStage->bundle[0].tcGen == TCGEN_ENVIRONMENT_MAPPED2 ) {
		tess.skinEnv = VK_SKIN_ENV_MAPPED2;
	} else {
		tess.skinEnv = VK_SKIN_ENV_NONE;
	}

	if ( pStage->rgbGen == CGEN_LIGHTING_SPHERICAL && tess.skinSphereLights != VK_SKIN_NONE ) {
		tess.skinLights = tess.skinSphereLights;
	} else {
		int numVertexes = tess.numVertexes;

		tess.numVertexes = 1;
		ComputeColors( pStage );
		tess.numVertexes = numVertexes;
		Com_Memcpy( &tess.skinColor, tess.svars.colors[0], 4 );
		tess.skinLights = VK_SKIN_NONE;
	}
}

/*
** RB_IterateStagesGeneric
*/
static void RB_IterateStagesGeneric( shaderCommands_t *input )
{
	int stage;

	for (stage = 0; stage < MAX_SHADER_STAGES; stage++)
	{
		shaderStage_t* pStage = tess.xstages[stage];
		qboolean directTc = qfalse, constColor = qfalse;

		if (!pStage)
		{
			break;
		}

		if (pStage->alphaGen == AGEN_SKYALPHA && backEnd.refdef.sky_alpha < 0.01) {
			continue;
		}
		else if (pStage->alphaGen == AGEN_ONE_MINUS_SKYALPHA && backEnd.refdef.sky_alpha > 0.99) {
			continue;
		}

		if (tess.staticBatch == STATIC_BATCH_ON) {
			RB_StaticStage(pStage);
		} else if (tess.staticBatch == STATIC_BATCH_SKINNED) {
			RB_SkinStage(pStage);
		} else {
			// on the CPU: the texture coordinates and colors that differ
			// from tess's own or from vertex to vertex
			directTc = RB_StageTexCoordsDirect(pStage);
			constColor = RB_StageConstantColor(pStage);
			if (!directTc) {
				ComputeTexCoords(pStage);
			}
			if (!constColor) {
				ComputeColors(pStage);
			}
		}

		if (constColor)
		{
			RB_SetStageConstantColor(pStage);
		}
		else if (!setArraysOnce)
		{
			RB_EnableColorArray(qtrue);
			RB_SetColorArray(input->svars.colors[0]);
		}

		//
		// do multitexture
		//
		if (pStage->bundle[1].image[0] != 0)
		{
			DrawMultitextured(input, stage, directTc);
		}
		else
		{
			if (backEnd.in2D) {
				R_State(pStage->stateBits | GLS_DEPTHTEST_DISABLE);
			}
			else {
				R_State(pStage->stateBits);
			}

			if (directTc)
			{
				RB_BindStageTexCoords(pStage, 0);
			}
			else if (!setArraysOnce)
			{
				RB_SetTexCoordArray(input->svars.texcoords[0][0], 0);
			}

			//
			// set state
			//
			if (pStage->bundle[0].vertexLightmap && r_vertexLight->integer && r_lightmap->integer)
			{
				R_Bind(tr.whiteImage);
			}
			else {
				R_BindAnimatedImage(&pStage->bundle[0]);
				if (pStage->bundle[0].isLightmap) {
					RB_LightmapDlights(0);
				}
			}

			//
			// draw
			//
			R_DrawElements(input->numVertexes, input->numIndexes, input->indexes);
			RB_LightmapDlights(-1);
		}
		// allow skipping out to show just lightmaps during development
		if (r_lightmap->integer && (pStage->bundle[0].isLightmap || pStage->bundle[1].isLightmap || pStage->bundle[0].vertexLightmap))
		{
			break;
		}
	}
}


/*
** RB_StageIteratorGeneric
*/
void RB_StageIteratorGeneric( void )
{
	shaderCommands_t *input;

	input = &tess;

	input->shaderTime = backEnd.refdef.floatTime;
	if ((input->shader->flags & BUNDLE_ANIMATE_ONCE) != 0) {
		input->shaderTime = backEnd.refdef.floatTime - backEnd.shaderStartTime;
	}

	// the effects shader deforms static batches (RB_StaticFxRecord); skinned
	// ones have no deforms (R_SkinPasses)
	if ( input->shader->numDeforms && tess.staticBatch != STATIC_BATCH_ON && tess.staticBatch != STATIC_BATCH_SKINNED ) {
		RB_DeformTessGeometry();
	}

	//
	// set face culling appropriately
	//
	R_Cull( input->shader->cullType );

	// set polygon offset if necessary
	if ( input->shader->polygonOffset )
	{
		vk_set_polygon_offset( qtrue, r_offsetFactor->value, r_offsetUnits->value );
	}

	//
	// if there is only a single pass then we can enable color
	// and texture arrays before we compile, otherwise we need
	// to avoid compiling those arrays since they will change
	// during multipass rendering
	//
	if (tess.numPasses > 1 || (tess.xstages[0] && tess.xstages[0]->multitextureEnv))
	{
		setArraysOnce = qfalse;
		RB_EnableColorArray(qfalse);
		RB_EnableTexCoordArray(qfalse);
	}
	else
	{
		setArraysOnce = qtrue;

		RB_EnableColorArray(qtrue);
		RB_SetColorArray(tess.svars.colors[0]);

		RB_EnableTexCoordArray(qtrue);
		RB_SetTexCoordArray(tess.svars.texcoords[0][0], 0);
	}

	//
	// set XYZ
	//
	RB_SetTessVertexArray();

	//
	// enable color and texcoord arrays if necessary
	//
	if ( !setArraysOnce )
	{
		RB_EnableTexCoordArray( qtrue );
		RB_EnableColorArray( qtrue );
	}

	//
	// call shader function
	//
	RB_IterateStagesGeneric( input );

	// 
	// now do any dynamic lighting needed
	//
	if (tess.dlightBits && tess.shader->sort <= SS_OPAQUE) {
		ProjectDlightTexture();
	}

	//
	// reset polygon offset
	//
	if ( input->shader->polygonOffset )
	{
		vk_set_polygon_offset( qfalse, 0, 0 );
	}
}


/*
** RB_StageIteratorVertexLitTexture
*/
void RB_StageIteratorVertexLitTextureUnfogged( void )
{
	shaderCommands_t *input;
	shader_t		*shader;

	input = &tess;

	shader = input->shader;

	if (tess.staticBatch == STATIC_BATCH_SKINNED)
	{
		// the vertex shader lights it (RB_SkinBatchAllowed)
	}
	else if (backEnd.currentSphere->TessFunction && r_drawspherelights->integer)
	{
		//
		// compute colors using the sphere function
		//
		backEnd.currentSphere->TessFunction((unsigned char*)tess.svars.colors);
	}
	else if (backEnd.currentStaticModel)
	{
		//
		// compute colors
		//
		if (tess.staticBatch == STATIC_BATCH_ON) {
			RB_StaticStage(input->xstages[0]);
		} else {
			ComputeColors(input->xstages[0]);
		}
	}
	else
	{
		//
		// calculate using previously calculated lighting grid
		// for the entity
		//
		RB_CalcLightGridColor((unsigned char*)tess.svars.colors);
	}

	//
	// set face culling appropriately
	//
	R_Cull( input->shader->cullType );

	//
	// set arrays
	//
	RB_EnableColorArray( qtrue );
	RB_EnableTexCoordArray( qtrue );

	RB_SetColorArray( tess.svars.colors[0] );
	RB_SetTexCoordArray( tess.texCoords[0][0], 16 );
	RB_SetTessVertexArray();

	//
	// call special shade routine
	//
	R_BindAnimatedImage( &tess.xstages[0]->bundle[0] );
	R_State( tess.xstages[0]->stateBits );
	R_DrawElements( input->numVertexes, input->numIndexes, input->indexes );

	// 
	// now do any dynamic lighting needed
	//
	if ( tess.dlightBits && tess.shader->sort <= SS_OPAQUE ) {
		ProjectDlightTexture();
	}
}

//define	REPLACE_MODE

void RB_StageIteratorLightmappedMultitextureUnfogged( void ) {
	shaderCommands_t *input;

	input = &tess;

	//
	// set face culling appropriately
	//
	R_Cull( input->shader->cullType );

	//
	// set color and pointers
	//
	R_State( GLS_DEFAULT | (tess.xstages[0]->stateBits & (GLS_ATEST_BITS | GLS_FOG_BITS)));
	RB_SetTessVertexArray();

	// white: the constant color (tr_pipelines.c predicts it), not an array
	RB_EnableColorArray( qfalse );
	RB_SetColor3f( 1, 1, 1 );

	//
	// select base stage
	//
	R_SelectTexture( 0 );

	RB_EnableTexCoordArray( qtrue );
	R_BindAnimatedImage( &tess.xstages[0]->bundle[0] );
	RB_SetTexCoordArray( tess.texCoords[0][0], 16 );

	//
	// configure second stage
	//
	R_SelectTexture( 1 );
	R_EnableTexture( qtrue );
	if ( r_lightmap->integer ) {
		R_TexEnv( TEXENV_REPLACE );
	} else {
		R_TexEnv( TEXENV_MODULATE );
	}
	R_BindAnimatedImage(&tess.xstages[0]->bundle[1]);
	if (tess.xstages[0]->bundle[1].isLightmap) {
		RB_LightmapDlights(1);
	}
	RB_EnableTexCoordArray( qtrue );
	RB_SetTexCoordArray( tess.texCoords[0][1], 16 );

	R_DrawElements( input->numVertexes, input->numIndexes, input->indexes );
	RB_LightmapDlights(-1);

	//
	// disable texturing on TEXTURE1, then select TEXTURE0
	//
	R_EnableTexture( qfalse );
	RB_EnableTexCoordArray( qfalse );

	R_SelectTexture( 0 );
#ifdef REPLACE_MODE
	R_TexEnv( TEXENV_MODULATE );
#endif

	// 
	// now do any dynamic lighting needed
	//
	if ( tess.dlightBits && tess.shader->sort <= SS_OPAQUE ) {
		ProjectDlightTexture();
	}
}

/*
** RB_EndSurface
*/
void RB_EndSurface( void ) {
	shaderCommands_t *input;

	input = &tess;

	if (input->numIndexes == 0) {
		tess.staticBatch = STATIC_BATCH_OFF;
		return;
	}
	if (input->indexes[SHADER_MAX_INDEXES-1] != 0) {
		ri.Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_INDEXES hit");
	}	
	if (input->xyz[SHADER_MAX_VERTEXES-1][0] != 0) {
		ri.Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_VERTEXES hit");
	}

	if (tess.xyz[SHADER_MAX_VERTEXES - 1][0] && r_shadows->integer != 3) {
		tess.xyz[SHADER_MAX_VERTEXES - 1][0] = 0;
	}

	if ( tess.shader == tr.shadowShader ) {
		RB_ComputeShadowVolume();
		return;
	}

	// for debugging of sort order issues, stop rendering after a given sort value
	if ( r_debugSort->integer && r_debugSort->integer < tess.shader->sort ) {
		return;
	}

	//
	// update performance counters
	//
	backEnd.pc.c_shaders++;
	backEnd.pc.c_vertexes += tess.numVertexes;
	backEnd.pc.c_indexes += tess.numIndexes;
	backEnd.pc.c_totalIndexes += tess.numIndexes * tess.numPasses;

	//
	// call off to shader specific tess end function
	//
	tess.currentStageIteratorFunc();

	if (!(backEnd.refdef.rdflags & RDF_NOWORLDMODEL) && !backEnd.in2D)
	{
		//
		// draw debugging stuff
		//
		if (r_showtris->integer && r_developer->integer) {
			DrawTris(input, r_showtris->integer);
		}
		if (r_shownormals->integer && r_developer->integer) {
			DrawNormals(input);
		}
	}

	// clear shader so we can tell we don't have any unclosed surfaces
	tess.numIndexes = 0;
	tess.staticBatch = STATIC_BATCH_OFF;
}

