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
#include "tr_local.h"

backEndData_t	*backEndData;
backEndData_t	*backEndDataArray[2];	// r_smp: the render thread reads one, the front end fills the other
backEndState_t	backEnd;

static float	s_flipMatrix[16] = {
	// convert from our coordinate system (looking down X)
	// to the eye space of the projection (looking down -Z)
	0, 0, -1, 0,
	-1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 0, 1
};

void R_SetFogColor(const vec4_t fColor) {
	glState.fFogColor[0] = fColor[0];
	glState.fFogColor[1] = fColor[1];
	glState.fFogColor[2] = fColor[2];
	glState.fFogColor[3] = fColor[3];

	if (!(glState.glStateBits & GLS_FOG_COLOR)) {
		vk_set_fog_color(glState.fFogColor);
	}
}

/*
** R_Bind
*/
void R_Bind( image_t *image ) {
	image_t *bound;

	if ( !image ) {
		ri.Printf( PRINT_WARNING, "R_Bind: NULL image\n" );
		bound = tr.defaultImage;
	} else {
		bound = image;
		image->UseCount++;
	}

	if ( r_nobind->integer && tr.dlightImage ) {		// performance evaluation option
		bound = tr.dlightImage;
	}

	if ( glState.currenttextures[glState.currenttmu] != bound->texnum ) {
		if (image) {
			image->frameUsed = tr.frameCount;
		}
		glState.currenttextures[glState.currenttmu] = bound->texnum;
		vk_bind_texture( glState.currenttmu, bound );
	}
}

/*
** R_SelectTexture
**
** Selects the texture unit used by R_Bind, R_TexEnv, R_EnableTexture
** and the texture coordinate array functions
*/
void R_SelectTexture( int unit )
{
	if ( glState.currenttmu == unit )
	{
		return;
	}

	if ( unit != 0 && unit != 1 ) {
		ri.Error( ERR_DROP, "R_SelectTexture: unit = %i", unit );
	}

	glState.currenttmu = unit;
}

/*
** R_EnableTexture
**
** Enables or disables texturing on the current texture unit
*/
void R_EnableTexture( qboolean enable ) {
	glState.texEnabled[glState.currenttmu] = enable;
}


/*
** R_Cull
*/
void R_Cull( int cullType ) {
	if ( glState.faceCulling == cullType ) {
		return;
	}

	glState.faceCulling = cullType;

	if ( cullType == CT_TWO_SIDED ) 
	{
		vk_set_cull( CULL_NONE );
	} 
	else 
	{
		if ( cullType == CT_BACK_SIDED )
		{
			if ( backEnd.viewParms.isMirror )
			{
				vk_set_cull( CULL_FRONT );
			}
			else
			{
				vk_set_cull( CULL_BACK );
			}
		}
		else
		{
			if ( backEnd.viewParms.isMirror )
			{
				vk_set_cull( CULL_BACK );
			}
			else
			{
				vk_set_cull( CULL_FRONT );
			}
		}
	}
}

/*
** R_TexEnv
*/
void R_TexEnv( int env )
{
	if ( env == glState.texEnv[glState.currenttmu] )
	{
		return;
	}

	glState.texEnv[glState.currenttmu] = env;


	switch ( env )
	{
	case TEXENV_MODULATE:
	case TEXENV_REPLACE:
	case TEXENV_DECAL:
	case TEXENV_ADD:
	case TEXENV_COMBINE_INTERPOLATE:
	case TEXENV_COMBINE4:
		vk_set_texenv( glState.currenttmu, env );
		break;
	default:
		ri.Error( ERR_DROP, "R_TexEnv: invalid env '%d' passed\n", env );
		break;
	}
}

/*
** R_State
**
** This routine is responsible for setting the most commonly changed state
** in Q3.
*/
void R_State( unsigned long stateBits )
{
	unsigned long diff = glState.glStateBits ^ (glState.externalSetState | stateBits);

	if ( !diff )
	{
		return;
	}

	//
	// blending, depth test/func/mask, polygon mode, alpha test and edge
	// clamping are all resolved by the backend from the state bits
	//
	vk_set_state( stateBits | glState.externalSetState );

	if ((diff & GLS_MULTITEXTURE_ENV) || (diff & GLS_MULTITEXTURE))
	{
		if (stateBits & GLS_MULTITEXTURE_ENV)
		{
			R_TexEnv(TEXENV_COMBINE4);
		}
		else if (stateBits & GLS_MULTITEXTURE)
		{
			glState.cntTexEnvExt = GLS_MULTITEXTURE;
			R_TexEnv(TEXENV_COMBINE_INTERPOLATE);
		}
		else
		{
			R_TexEnv(TEXENV_MODULATE);
		}
	}
	
	if (diff & GLS_FOG)
	{
		if (glState.externalSetState & GLS_FOG)
		{
			vk_set_fog(qtrue);

			if (stateBits & GLS_FOG_BLACK)
			{
				vec4_t fBlackFogColor = { 0, 0, 0, 1 };
				vk_set_fog_color(fBlackFogColor);
			}
			else if (stateBits & GLS_FOG_WHITE)
			{
				vec4_t fWhiteFogColor = { 1, 1, 1, 1 };
				vk_set_fog_color(fWhiteFogColor);
			}
			else
			{
				//
				// Use the global fog
				vk_set_fog_color(glState.fFogColor);
			}
		}
		else
		{
			vk_set_fog(qfalse);
		}
	}
	else if (glState.externalSetState & GLS_FOG)
	{
		if (diff & GLS_FOG_ENABLED)
		{
			vk_set_fog((stateBits & GLS_FOG_ENABLED) != 0);
		}

		if (diff & GLS_FOG_COLOR)
		{
			if (stateBits & GLS_FOG_BLACK)
			{
				vec4_t fBlackFogColor = { 0, 0, 0, 1 };
				vk_set_fog_color(fBlackFogColor);
			}
			else if (stateBits & GLS_FOG_WHITE)
			{
				vec4_t fWhiteFogColor = { 1, 1, 1, 1 };
				vk_set_fog_color(fWhiteFogColor);
			}
			else
            {
                //
                // Use the global fog
                vk_set_fog_color(glState.fFogColor);
            }
		}
	}

	glState.glStateBits = stateBits | glState.externalSetState;
}

/*
============================================================================

MATRICES, VIEWPORT AND VERTEX ARRAYS

============================================================================
*/

/*
** RB_SetProjection
*/
void RB_SetProjection( const float *matrix ) {
	Com_Memcpy( glState.projection, matrix, sizeof( glState.projection ) );
	vk_set_projection( glState.projection );
}

/*
** RB_SetModelview
*/
void RB_SetModelview( const float *matrix ) {
	Com_Memcpy( glState.modelview, matrix, sizeof( glState.modelview ) );
	vk_set_modelview( glState.modelview );
}

/*
** RB_LoadIdentity
*/
void RB_LoadIdentity( float *matrix ) {
	Com_Memset( matrix, 0, sizeof( float ) * 16 );
	matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

/*
** RB_OrthoMatrix
**
** Same matrix as glOrtho applied to the identity
*/
void RB_OrthoMatrix( float *matrix, float left, float right, float bottom, float top, float zNear, float zFar ) {
	RB_LoadIdentity( matrix );
	matrix[0] = 2.0f / ( right - left );
	matrix[5] = 2.0f / ( top - bottom );
	matrix[10] = -2.0f / ( zFar - zNear );
	matrix[12] = -( right + left ) / ( right - left );
	matrix[13] = -( top + bottom ) / ( top - bottom );
	matrix[14] = -( zFar + zNear ) / ( zFar - zNear );
}

/*
** RB_TranslateModelview
**
** Post-multiplies the modelview matrix by a translation, like glTranslatef
*/
void RB_TranslateModelview( float x, float y, float z ) {
	float *m = glState.modelview;
	int i;

	for ( i = 0; i < 4; i++ ) {
		m[12 + i] += m[i] * x + m[4 + i] * y + m[8 + i] * z;
	}
	vk_set_modelview( m );
}

/*
** RB_SetViewport
*/
void RB_SetViewport( int x, int y, int width, int height ) {
	vk_set_viewport( x, y, width, height );
}

/*
** RB_SetFogRange
*/
void RB_SetFogRange( float start, float end ) {
	glState.fogStart = start;
	glState.fogEnd = end;
	vk_set_fog_range( start, end );
}

/*
** RB_SetScissor
*/
void RB_SetScissor( int x, int y, int width, int height ) {
	vk_set_scissor( x, y, width, height );
}

/*
** RB_SetColor4f
**
** Sets the current color, used by immediate mode vertexes and by draws
** without a color array
*/
void RB_SetColor4f( float r, float g, float b, float a ) {
	glState.color[0] = Com_Clamp( 0, 1, r );
	glState.color[1] = Com_Clamp( 0, 1, g );
	glState.color[2] = Com_Clamp( 0, 1, b );
	glState.color[3] = Com_Clamp( 0, 1, a );
}

/*
** RB_SetColor3f
*/
void RB_SetColor3f( float r, float g, float b ) {
	RB_SetColor4f( r, g, b, 1.0f );
}

/*
** RB_SetColor4ubv
*/
void RB_SetColor4ubv( const byte *rgba ) {
	glState.color[0] = rgba[0] / 255.0f;
	glState.color[1] = rgba[1] / 255.0f;
	glState.color[2] = rgba[2] / 255.0f;
	glState.color[3] = rgba[3] / 255.0f;
}

/*
** RB_SetVertexArray
**
** A stride of 0 means tightly packed
*/
void RB_SetVertexArray( const float *xyz, int stride ) {
	glState.arrays.xyz = xyz;
	glState.arrays.xyzStride = stride ? stride : sizeof( float ) * 3;
	glState.arrays.xyzVersion = 0;
}

/*
** RB_SetTessVertexArray
**
** Sets tess.xyz as the vertex array for the draws of a stage iterator. The
** positions and indexes don't change until the iterator returns, so the
** draws of its passes get the same version and the backend copies the
** positions (and the indexes, when they are the same array) only once.
*/
void RB_SetTessVertexArray( void ) {
	static unsigned int version;

	glState.arrays.xyz = tess.xyz[0];
	glState.arrays.xyzStride = 16;	// padded for SIMD
	if ( !++version ) {
		version = 1;
	}
	glState.arrays.xyzVersion = version;
}

/*
** RB_SetColorArray
*/
void RB_SetColorArray( const byte *colors ) {
	glState.arrays.colors = colors;
}

/*
** RB_EnableColorArray
*/
void RB_EnableColorArray( qboolean enable ) {
	glState.arrays.colorsEnabled = enable;
}

/*
** RB_SetTexCoordArray
**
** Sets the texture coordinates of the current texture unit;
** a stride of 0 means tightly packed
*/
void RB_SetTexCoordArray( const float *st, int stride ) {
	glState.arrays.st[glState.currenttmu] = st;
	glState.arrays.stStride[glState.currenttmu] = stride ? stride : sizeof( float ) * 2;
}

/*
** RB_EnableTexCoordArray
**
** Enables or disables the texture coordinates of the current texture unit
*/
void RB_EnableTexCoordArray( qboolean enable ) {
	glState.arrays.stEnabled[glState.currenttmu] = enable;
}

/*
** RB_ShaderDepthClamp
**
** Whether the surfaces of a shader can be drawn with depth clamp
** (vk_set_depth_clamp). Near clipping cuts away the part of a surface that is
** nearer than r_znear, so a view leaning into a wall would see through it, or
** a pane of glass would end in a hard edge at the near plane; depth clamp
** keeps the surface whole. That goes for the solid surfaces (they write
** depth) and for the translucent ones of the map (glass, marks), but not for
** sprites and effects, which a view passes through: they keep the near
** clipping, or one in front of the camera would fill the screen. The shader
** decides, so that every pass of a surface gets the same, or the depth equal
** passes after the first one wouldn't match its depth where it is clamped
*/
typedef enum { RB_CLAMP_NONE, RB_CLAMP_SOLID, RB_CLAMP_TRANSLUCENT } rbClampClass_t;

static rbClampClass_t RB_ShaderClampClass( const shader_t *shader ) {
	int i;

	if ( !shader || shader->isSky || shader->sort < SS_OPAQUE ) {
		return RB_CLAMP_NONE;
	}
	if ( shader->sort <= SS_BANNER ) {
		return RB_CLAMP_SOLID;
	}
	if ( shader->sort >= SS_UNDERWATER && shader->sort <= SS_BLEND6 ) {
		// a sort of a shader file, or the default of blended stages: still
		// solid when a stage writes depth
		for ( i = 0; i < MAX_SHADER_STAGES && shader->unfoggedStages[i]; i++ ) {
			if ( ( shader->unfoggedStages[i]->stateBits & GLS_DEPTHMASK_TRUE ) && !( shader->unfoggedStages[i]->stateBits & GLS_DEPTHTEST_DISABLE ) ) {
				return RB_CLAMP_SOLID;
			}
		}
		return RB_CLAMP_TRANSLUCENT;
	}
	return RB_CLAMP_NONE;
}

qboolean RB_ShaderDepthClamp( const shader_t *shader ) {
	return r_depthClamp->integer && RB_ShaderClampClass( shader ) != RB_CLAMP_NONE;
}

// the batch being drawn is of sprites (RB_RenderSpriteSurfList, SF_SPRITE)
static qboolean rb_spriteBatch;
// the flares drawn in a 3D view: reactive whatever entity came before
// (RB_TemporalReactive)
static qboolean rb_flareDraws;

/*
** RB_DrawClamped
**
** r_depthClamp 1: the world's own surfaces (brushes, terrain, polys). 2: also
** static models and entities (brush models, models), whose translucent
** surfaces stay unclamped
*/
static qboolean RB_DrawClamped( void ) {
	rbClampClass_t	class;
	qboolean		world;

	if ( backEnd.in2D || ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) || rb_spriteBatch ) {
		return qfalse;
	}
	class = r_depthClamp->integer ? RB_ShaderClampClass( tess.shader ) : RB_CLAMP_NONE;
	if ( class == RB_CLAMP_NONE ) {
		return qfalse;
	}
	world = !backEnd.currentStaticModel && backEnd.currentEntity == &tr.worldEntity;
	if ( world ) {
		return qtrue;
	}
	if ( r_depthClamp->integer < 2 ) {
		return qfalse;
	}
	// static models also have glass and the like; other entities' translucent
	// surfaces are effects
	return class == RB_CLAMP_SOLID || backEnd.currentStaticModel != NULL;
}

/*
** RB_DrawElements
**
** Draws triangles from the current vertex arrays
*/
static void RB_SubmitElements( int numVertexes, int numIndexes, const glIndex_t *indexes );

void RB_DrawElements( int numVertexes, int numIndexes, const glIndex_t *indexes ) {
	// the view weapon's depth hack turns it off in vk_draw
	vk_set_depth_clamp( RB_DrawClamped() );
	// the temporal path's reactive effects (sprites included)
	vk_set_reactive( RB_TemporalReactive( rb_flareDraws || rb_spriteBatch ) );
	RB_SubmitElements( numVertexes, numIndexes, indexes );
	// not for the immediate mode draws
	vk_set_depth_clamp( qfalse );
	vk_set_reactive( qfalse );
}

static void RB_SubmitElements( int numVertexes, int numIndexes, const glIndex_t *indexes ) {
	drawGeometry_t geometry;
	int i;

	geometry.numVertexes = numVertexes;
	geometry.isSkinned = qfalse;
	geometry.staticTerrain = qfalse;
	geometry.xyz = glState.arrays.xyz;
	geometry.xyzStride = glState.arrays.xyzStride;
	geometry.version = glState.arrays.xyzVersion;
	geometry.colors = glState.arrays.colorsEnabled ? glState.arrays.colors : NULL;
	Vector4Copy( glState.color, geometry.color );
	// the temporal path's normals buffer: the surfaces fill tess.normal in
	// its scene
	if ( backEnd.temporalScene && geometry.xyz == tess.xyz[0] ) {
		geometry.normals = tess.normal[0];
		geometry.normalStride = sizeof( tess.normal[0] );
	} else {
		geometry.normals = NULL;
		geometry.normalStride = 0;
	}

	for ( i = 0; i < NUM_TEXTURE_BUNDLES; i++ ) {
		if ( glState.texEnabled[i] && glState.arrays.stEnabled[i] && glState.arrays.st[i] ) {
			geometry.st[i] = glState.arrays.st[i];
			geometry.stStride[i] = glState.arrays.stStride[i];
		} else {
			geometry.st[i] = NULL;
			geometry.stStride[i] = 0;
		}
	}

	// the stage iterators drawing a batch of the static geometry, or a
	// skinned batch
	if ( tess.staticBatch == STATIC_BATCH_ON && geometry.xyz == tess.xyz[0] && indexes == tess.indexes ) {
		RB_StaticDraw( &geometry );
		return;
	}
	if ( tess.staticBatch == STATIC_BATCH_SKINNED && geometry.xyz == tess.xyz[0] && indexes == tess.indexes ) {
		RB_SkinDraw( &geometry );
		return;
	}

	geometry.isStatic = qfalse;
	vk_draw( PRIM_TRIANGLES, &geometry, numIndexes, indexes );
}



/*
================
RB_Hyperspace

A player has predicted a teleport, but hasn't arrived yet
================
*/
static void RB_Hyperspace( void ) {
	float		c;

	if ( !backEnd.isHyperspace ) {
		// do initialization shit
	}

	c = ( backEnd.refdef.time & 255 ) / 255.0f;
	{
		vec4_t clearColor = { c, c, c, 1 };
		vk_clear( CLEAR_COLOR, clearColor );
	}

	backEnd.isHyperspace = qtrue;
}


static void SetViewportAndScissor( void ) {
	RB_SetProjection( backEnd.viewParms.projectionMatrix );

	// set the window clipping
	RB_SetViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, 
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	RB_SetScissor( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, 
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
}

/*
=================
RB_BeginDrawingView

Any mirrored or portaled views have already been drawn, so prepare
to actually render the visible surfaces for this view
=================
*/
void RB_BeginDrawingView (void) {
	int clearBits = 0;
	vec4_t clearColor = { 0, 0, 0, 1 };

	// sync with the gpu if needed
	if ( r_finish->integer == 1 && !glState.finishCalled ) {
		vk_finish ();
		glState.finishCalled = qtrue;
	}
	if ( r_finish->integer == 0 ) {
		glState.finishCalled = qtrue;
	}

	// we will need to change the projection matrix before drawing
	// 2D images again
	backEnd.in2D = qfalse;

	//
	// set the modelview matrix for the viewer
	//
	SetViewportAndScissor();

	// ensures that depth writes are enabled for the depth clear
	R_State( GLS_DEFAULT );
	// clear relevant buffers
	clearBits = CLEAR_DEPTH;

	if ( r_measureOverdraw->integer || r_shadows->integer == 3 )
	{
		clearBits |= CLEAR_STENCIL;
	}

	if (!( backEnd.refdef.rdflags & RDF_NOWORLDMODEL))
	{
		if ((backEnd.viewParms.farplane_distance && !backEnd.viewParms.skyRendered && !backEnd.viewParms.portalRendered) || backEnd.viewParms.farclip)
		{
			clearBits |= CLEAR_COLOR;
			Vector4Copy(glState.fFogColor, clearColor);
		}
		else
		{
			if (r_fastsky->integer)
			{
				clearBits |= CLEAR_COLOR;
				VectorSet4(clearColor, 0.5, 0.5, 1.0, 1.0);
			}
		}
	}
	vk_clear( clearBits, clearColor );

	if ( ( backEnd.refdef.rdflags & RDF_HYPERSPACE ) )
	{
		RB_Hyperspace();
		return;
	}
	else
	{
		backEnd.isHyperspace = qfalse;
	}

	glState.faceCulling = -1;		// force face culling to set next time

	// we will only draw a sun if there was sky rendered in this view
	backEnd.skyRenderedThisView = qfalse;

	// clip to the plane of the portal
	if ( backEnd.viewParms.isPortal ) {
		float	plane[4];
		float	plane2[4];
		float	eyePlane[4];
		int		i;

		plane[0] = backEnd.viewParms.portalPlane.normal[0];
		plane[1] = backEnd.viewParms.portalPlane.normal[1];
		plane[2] = backEnd.viewParms.portalPlane.normal[2];
		plane[3] = backEnd.viewParms.portalPlane.dist;

		plane2[0] = DotProduct (backEnd.viewParms.ori.axis[0], plane);
		plane2[1] = DotProduct (backEnd.viewParms.ori.axis[1], plane);
		plane2[2] = DotProduct (backEnd.viewParms.ori.axis[2], plane);
		plane2[3] = DotProduct (plane, backEnd.viewParms.ori.origin) - plane[3];

		RB_SetModelview( s_flipMatrix );

		// the plane is specified relative to s_flipMatrix: bring it to eye
		// space with the inverse of that rotation (its transpose)
		for ( i = 0; i < 3; i++ ) {
			eyePlane[i] = plane2[0] * s_flipMatrix[i] + plane2[1] * s_flipMatrix[4 + i] + plane2[2] * s_flipMatrix[8 + i];
		}
		eyePlane[3] = plane2[3];

		vk_set_clip_plane( eyePlane );
	} else {
		vk_set_clip_plane( NULL );
	}
}

/*
==================
RB_AddSurface

Only some surface types can be drawn in a batch of the static geometry
==================
*/
static ID_INLINE void RB_AddSurface( surfaceType_t *surface ) {
	if ( tess.staticBatch == STATIC_BATCH_ON && !RB_StaticSurface( surface ) ) {
		RB_StaticBatchToCPU();
	}
	rb_surfaceTable[ *surface ]( surface );
}

/*
==================
RB_RenderDrawSurfList
==================
*/
void RB_RenderDrawSurfList( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	shader_t		*shader, *oldShader;
	int				entityNum, oldEntityNum;
	int				dlightMap, oldDlightMap;
	qboolean		depthRange, oldDepthRange;
	qboolean		bStaticModel, oldbStaticModel;
	int				i;
	drawSurf_t		*drawSurf;
	int				oldSort;
	float			originalTime;

	// save original time for entity shader offsets
	originalTime = backEnd.refdef.floatTime;

	// clear the z buffer, set the modelview, etc
	RB_BeginDrawingView ();

	// draw everything
	oldEntityNum = -1;
	backEnd.currentEntity = &tr.worldEntity;
	oldShader = NULL;
	oldDepthRange = qfalse;
	oldDlightMap = 0;
	oldSort = -1;
	oldbStaticModel = -1;
	depthRange = qfalse;

	backEnd.pc.c_surfaces += numDrawSurfs;
	backEnd.numSpheresUsed = 0;
	backEnd.dlightList = backEnd.worldDlightList = -1;

	for (i = 0, drawSurf = drawSurfs ; i < numDrawSurfs ; i++, drawSurf++) {
		if ( drawSurf->sort == (unsigned)oldSort && *drawSurf->surface != SF_SPRITE ) {
			// fast path, same as previous sort
			RB_AddSurface( drawSurf->surface );
			continue;
		}
		oldSort = (int)drawSurf->sort;
		if (*drawSurf->surface != SF_SPRITE) {
			R_DecomposeSort(drawSurf->sort, &entityNum, &shader, &dlightMap, &bStaticModel);
		} else {
			shader = tr.sortedShaders[((refSprite_t*)drawSurf->surface)->shaderNum];
			entityNum = ENTITYNUM_WORLD;
			dlightMap = 0;
			bStaticModel = qfalse;
		}

		//
		// change the tess parameters if needed
		// a "entityMergable" shader is a shader that can have surfaces from seperate
		// entities merged into a single batch, like smoke and blood puff sprites
		if (shader != oldShader || dlightMap != oldDlightMap || (oldShader->flags & 1)
			|| ( entityNum != oldEntityNum && !shader->entityMergable )
			|| ( bStaticModel != oldbStaticModel && !shader->entityMergable )) {
			// static models drawn from the static geometry with the same
			// shader can share a batch (tr_staticgeom.c)
			if (shader != oldShader || dlightMap != oldDlightMap || (oldShader->flags & 1)
				|| !bStaticModel || !oldbStaticModel || !RB_StaticMerge(entityNum)) {
				if (oldShader != NULL) {
					RB_EndSurface();
				}
				// the temporal path's ambient occlusion: after the opaque
				// surfaces, decals and see-through ones, before what is
				// blended over them (tr_temporal.c)
				if (shader->sort >= SS_UNDERWATER && backEnd.temporalPrimary) {
					RB_TemporalAo();
				}
				rb_spriteBatch = *drawSurf->surface == SF_SPRITE;
				RB_BeginSurface( shader );
				oldShader = shader;
				oldDlightMap = dlightMap;
			}
		}

		backEnd.currentSphere = &backEnd.spareSphere;
		//
		// change the modelview matrix if needed
		//
		if ( entityNum != oldEntityNum || bStaticModel != oldbStaticModel ) {
			depthRange = qfalse;

			if (bStaticModel) {
				backEnd.shaderStartTime = 0.0;
				backEnd.currentEntity = 0;
				backEnd.spareSphere.TessFunction = 0;
				backEnd.currentStaticModel = &backEnd.refdef.staticModels[entityNum];
				R_RotateForStaticModel(backEnd.currentStaticModel, &backEnd.viewParms, &backEnd.ori );
			}
			else {
				backEnd.currentStaticModel = NULL;
				if (entityNum != ENTITYNUM_WORLD) {
					backEnd.currentEntity = &backEnd.refdef.entities[entityNum];
					backEnd.shaderStartTime = backEnd.currentEntity->e.shaderTime;

					// set up the transformation matrix
					R_RotateForEntity(backEnd.currentEntity, &backEnd.viewParms, &backEnd.ori);

					// set up the dynamic lighting if needed
					if (backEnd.currentEntity->needDlights) {
						RB_TransformDlights(qfalse);
					}

					if (backEnd.currentEntity->e.renderfx & RF_DEPTHHACK) {
						// hack the depth range to prevent view model from poking into walls
						depthRange = qtrue;
					}
				}
				else {
					backEnd.currentEntity = &tr.worldEntity;
					backEnd.ori = backEnd.viewParms.world;
					backEnd.shaderStartTime = 0.0;
					RB_TransformDlights(qtrue);
				}
			}

			RB_SetModelview( backEnd.ori.modelMatrix );
			// the temporal path's motion of an entity (tr_temporal.c)
			RB_TemporalModelview();

			//
			// change depthrange if needed
			//
			if ( oldDepthRange != depthRange ) {
				if ( depthRange ) {
					vk_set_depth_range (0, VK_DEPTH_HACK_MAX);
				} else {
					vk_set_depth_range (0, 1.0);
				}
				oldDepthRange = depthRange;
			}

			oldEntityNum = entityNum;
			oldbStaticModel = bStaticModel;
		}

		if (bStaticModel)
		{
			if (r_drawspherelights->integer) {
				RB_Static_BuildDLights();
			}

			if (!backEnd.currentStaticModel->bLightGridCalculated) {
				RB_Grid_SetupStaticModel();
			}
		}
		else if (backEnd.currentEntity->e.tiki)
		{
			if (shader->needsLGrid
				|| shader->needsLSpherical && r_fastentlight->integer
				|| !r_drawspherelights->integer)
			{
				backEnd.currentSphere->TessFunction = RB_CalcLightGridColor;
				RB_Grid_SetupEntity();
			}
			else if (shader->needsLSpherical)
			{
				if (backEnd.refdef.rdflags & RDF_HUD)
				{
					backEnd.currentSphere = &backEnd.hudSphere;
					backEnd.hudSphere.TessFunction = 0;
					RB_Sphere_SetupEntity();
				}
				else if (backEnd.currentEntity->sphereCalculated)
				{
					backEnd.currentSphere = &backEnd.spheres[backEnd.currentEntity->lightingSphere];
				}
				else
				{
					if (backEnd.numSpheresUsed == MAX_SPHERE_LIGHTS)
					{
						ri.Printf(PRINT_DEVELOPER, "Spherical lighting: Ran out of space in the sphere array!\n");
						backEnd.currentSphere = &backEnd.spareSphere;
					}
					else
					{
						backEnd.currentSphere = &backEnd.spheres[backEnd.numSpheresUsed];
						backEnd.currentEntity->lightingSphere = backEnd.numSpheresUsed++;
						backEnd.currentEntity->sphereCalculated = 1;
					}

					backEnd.currentSphere->TessFunction = NULL;
					RB_Sphere_SetupEntity();
				}
			}
		}

		if (*drawSurf->surface == SF_SPRITE) {
			backEnd.shaderStartTime = ((refSprite_t*)drawSurf->surface)->shaderTime;
		}

		// add the triangles for this surface
		RB_AddSurface( drawSurf->surface );
	}

	// draw the contents of the last shader batch
	if (oldShader != NULL) {
		RB_EndSurface();
	}

	// go back to the world modelview matrix
	RB_SetModelview( backEnd.viewParms.world.modelMatrix );
	if ( depthRange ) {
		vk_set_depth_range (0, 1.0);
	}

	// the ambient occlusion of a view without blended surfaces
	if ( backEnd.temporalPrimary ) {
		RB_TemporalAo();
	}

	RB_ShadowFinish();
	if (!(backEnd.refdef.rdflags & RDF_HUD)) {
		rb_flareDraws = qtrue;
		RB_RenderFlares();
		R_DrawLensFlares();
		rb_flareDraws = qfalse;
	}

	if (g_bInfoworldtris)
	{
		g_bInfoworldtris = 0;
		R_PrintInfoWorldtris();
	}

#if 0
	RB_DrawSun();
#endif
	// darken down any stencil shadows
	RB_ShadowFinish();		

	// add light flares on lights that aren't obscured
	rb_flareDraws = qtrue;
	RB_RenderFlares();
	rb_flareDraws = qfalse;
}

/*
==================
RB_RenderSpriteSurfList
==================
*/
void RB_RenderSpriteSurfList(drawSurf_t* drawSurfs, int numDrawSurfs) {
	shader_t	*shader;
	shader_t	*oldShader;
	qboolean	depthRange;
	qboolean	oldDepthRange;
	int			i;
	drawSurf_t	*drawSurf;

    backEnd.currentEntity = &tr.worldEntity;
    backEnd.currentStaticModel = 0;
	rb_spriteBatch = qtrue;

	backEnd.pc.c_surfaces += numDrawSurfs;
	backEnd.ori = backEnd.viewParms.world;

	oldShader = NULL;
    depthRange = qfalse;
    oldDepthRange = qfalse;

    for (i = 0, drawSurf = drawSurfs; i < numDrawSurfs; i++, drawSurf++) {
		shader = tr.sortedShaders[((refSprite_t*)drawSurf->surface)->shaderNum];
		depthRange = (((refSprite_t*)drawSurf->surface)->renderfx & RF_DEPTHHACK) != 0;

        if ((shader != oldShader || (oldShader->flags & RF_THIRD_PERSON) != 0) && !shader->entityMergable)
        {
			if (oldShader) {
				RB_EndSurface();
			}

            RB_BeginSurface(shader);
            oldShader = shader;
        }

        RB_SetModelview(backEnd.ori.modelMatrix);

        if (oldDepthRange != depthRange)
        {
			if (depthRange) {
				vk_set_depth_range(0.0, VK_DEPTH_HACK_MAX);
			} else {
                vk_set_depth_range(0.0, 1.0);
			}

            oldDepthRange = depthRange;
        }

        backEnd.shaderStartTime = ((refSprite_t*)drawSurf->surface)->shaderTime;

        // add the triangles for this surface
        rb_surfaceTable[*drawSurf->surface](drawSurf->surface);
	}

	if (oldShader) {
		RB_EndSurface();
	}

	rb_spriteBatch = qfalse;

    // go back to the world modelview matrix
    RB_SetModelview(backEnd.viewParms.world.modelMatrix);
	// go back to the previous depth range
	if (depthRange) {
		vk_set_depth_range(0.0, 1.0);
	}
}


/*
============================================================================

RENDER BACK END THREAD FUNCTIONS

============================================================================
*/

/*
================
RB_SetGL2D

================
*/
void	RB_SetGL2D (void) {
	Set2DWindow(0, 0, glConfig.vidWidth, glConfig.vidHeight, 0.0, glConfig.vidWidth, glConfig.vidHeight, 0.0, 0.0, 1.0);
}


/*
=============
RE_StretchRaw

FIXME: not exactly backend
Stretches a raw 32 bit power of 2 bitmap image over the given screen rectangle.
Used for cinematics.
=============
*/
void RE_StretchRaw (int x, int y, int w, int h, int cols, int rows, int components, const byte* data) {
	int			i, j;
	int			start, end;

	if ( !tr.registered ) {
		return;
	}
	R_IssuePendingRenderCommands();

	// we definately want to sync every frame for the cinematics
	vk_finish();

	start = end = 0;
	if ( r_speeds->integer ) {
		start = ri.Milliseconds();
	}

	// make sure rows and cols are powers of 2
	for ( i = 0 ; ( 1 << i ) < cols ; i++ ) {
	}
	for ( j = 0 ; ( 1 << j ) < rows ; j++ ) {
	}
	if ( ( 1 << i ) != cols || ( 1 << j ) != rows) {
		ri.Error (ERR_DROP, "Draw_StretchRaw: size not a power of 2: %i by %i", cols, rows);
	}

	R_Bind( tr.scratchImage );

	// if the scratchImage isn't in the format we want, specify it as a new texture
	R_UploadScratchImage( tr.scratchImage, cols, rows, data, qtrue );

	if ( r_speeds->integer ) {
		end = ri.Milliseconds();
		ri.Printf( PRINT_ALL, "texture upload %i, %i: %i msec\n", cols, rows, end - start );
	}

	RB_SetGL2D();

	RB_SetColor3f( tr.identityLight, tr.identityLight, tr.identityLight );

	RB_ImmBegin (IMM_QUADS);
	RB_ImmTexCoord2f ( 0.5f / cols,  0.5f / rows );
	RB_ImmVertex2f (x, y);
	RB_ImmTexCoord2f ( ( cols - 0.5f ) / cols ,  0.5f / rows );
	RB_ImmVertex2f (x+w, y);
	RB_ImmTexCoord2f ( ( cols - 0.5f ) / cols, ( rows - 0.5f ) / rows );
	RB_ImmVertex2f (x+w, y+h);
	RB_ImmTexCoord2f ( 0.5f / cols, ( rows - 0.5f ) / rows );
	RB_ImmVertex2f (x, y+h);
	RB_ImmEnd ();
}

void RE_UploadCinematic (int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty) {
	// the open 2D batch is drawn with the contents it was made with
	R_FrontEndSync();
	R_End2DBatch();

	R_Bind( &tr.scratchImage[client] );

	// if the scratchImage isn't in the format we want, specify it as a new texture
	R_UploadScratchImage( &tr.scratchImage[client], cols, rows, data, dirty );
}

/*
=============
RB_SetColor

=============
*/
const void	*RB_SetColor( const void *data ) {
	const setColorCommand_t	*cmd;

	cmd = (const setColorCommand_t *)data;

	backEnd.color2D[0] = cmd->color[0] * 255;
	backEnd.color2D[1] = cmd->color[1] * 255;
	backEnd.color2D[2] = cmd->color[2] * 255;
	backEnd.color2D[3] = cmd->color[3] * 255;

	return (const void *)(cmd + 1);
}

/*
=============
RB_StretchPic
=============
*/
const void *RB_StretchPic ( const void *data ) {
	const stretchPicCommand_t	*cmd;
	shader_t *shader;
	int		numVerts, numIndexes;

	cmd = (const stretchPicCommand_t *)data;

	if ( !backEnd.in2D ) {
		RB_SetGL2D();
	}

	shader = cmd->shader;
	if ( shader != tess.shader ) {
		if ( tess.numIndexes ) {
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface( shader );
	}

	RB_CHECKOVERFLOW( 4, 6 );
	numVerts = tess.numVertexes;
	numIndexes = tess.numIndexes;

	tess.numVertexes += 4;
	tess.numIndexes += 6;

	tess.indexes[ numIndexes ] = numVerts + 3;
	tess.indexes[ numIndexes + 1 ] = numVerts + 0;
	tess.indexes[ numIndexes + 2 ] = numVerts + 2;
	tess.indexes[ numIndexes + 3 ] = numVerts + 2;
	tess.indexes[ numIndexes + 4 ] = numVerts + 0;
	tess.indexes[ numIndexes + 5 ] = numVerts + 1;

	*(int *)tess.vertexColors[ numVerts ] =
		*(int *)tess.vertexColors[ numVerts + 1 ] =
		*(int *)tess.vertexColors[ numVerts + 2 ] =
		*(int *)tess.vertexColors[ numVerts + 3 ] = *(int *)backEnd.color2D;

	tess.xyz[ numVerts ][0] = cmd->x;
	tess.xyz[ numVerts ][1] = cmd->y;
	tess.xyz[ numVerts ][2] = 0;

	tess.texCoords[ numVerts ][0][0] = cmd->s1;
	tess.texCoords[ numVerts ][0][1] = cmd->t1;

	tess.xyz[ numVerts + 1 ][0] = cmd->x + cmd->w;
	tess.xyz[ numVerts + 1 ][1] = cmd->y;
	tess.xyz[ numVerts + 1 ][2] = 0;

	tess.texCoords[ numVerts + 1 ][0][0] = cmd->s2;
	tess.texCoords[ numVerts + 1 ][0][1] = cmd->t1;

	tess.xyz[ numVerts + 2 ][0] = cmd->x + cmd->w;
	tess.xyz[ numVerts + 2 ][1] = cmd->y + cmd->h;
	tess.xyz[ numVerts + 2 ][2] = 0;

	tess.texCoords[ numVerts + 2 ][0][0] = cmd->s2;
	tess.texCoords[ numVerts + 2 ][0][1] = cmd->t2;

	tess.xyz[ numVerts + 3 ][0] = cmd->x;
	tess.xyz[ numVerts + 3 ][1] = cmd->y + cmd->h;
	tess.xyz[ numVerts + 3 ][2] = 0;

	tess.texCoords[ numVerts + 3 ][0][0] = cmd->s1;
	tess.texCoords[ numVerts + 3 ][0][1] = cmd->t2;

	return (const void *)(cmd + 1);
}

/*
=============
RB_SetupFog
=============
*/
void RB_SetupFog() {
	backEnd.setupFogs++;
	if (!backEnd.viewParms.farplane_distance) {
		glState.externalSetState &= ~GLS_FOG;
	} else if (r_farplane_nofog->integer) {
		glState.externalSetState &= ~GLS_FOG;
	} else {
		vec4_t vFogColor;

		RB_SetFogRange(backEnd.viewParms.farplane_bias, backEnd.viewParms.farplane_distance);

        vFogColor[0] = backEnd.viewParms.farplane_color[0] * tr.identityLight;
        vFogColor[1] = backEnd.viewParms.farplane_color[1] * tr.identityLight;
        vFogColor[2] = backEnd.viewParms.farplane_color[2] * tr.identityLight;
        vFogColor[3] = 1.0;
        R_SetFogColor(vFogColor);

		glState.externalSetState |= GLS_FOG;
	}
}

/*
=============
RB_DrawSurfs

=============
*/
const void	*RB_DrawSurfs( const void *data ) {
	const drawSurfsCommand_t	*cmd;

	// finish any 2D drawing if needed
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	cmd = (const drawSurfsCommand_t *)data;

	// a scene: not 2D (RE_RenderScene sets it without the render thread)
	backEnd.in2D = qfalse;
	backEnd.refdef = cmd->refdef;
	backEnd.viewParms = cmd->viewParms;
	// r_smp: the terrain slots this scene changed (RB_TerrainPool)
	backEnd.terrainChanges = cmd->terrainChanges;
	backEnd.numTerrainChanges = cmd->numTerrainChanges;
	backEnd.terrainChangesAll = cmd->terrainChangesAll;

	// the temporal path's scene (tr_temporal.c)
	RB_TemporalBeginView();
	RB_SetupFog();
	RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );

	return (const void *)(cmd + 1);
}


/*
=============
RB_DrawSurfs

=============
*/
const void* RB_SpriteSurfs(const void* data) {
	backEnd.in2D = qfalse;
    const drawSurfsCommand_t* cmd;

    // finish any 2D drawing if needed
    if (tess.numIndexes) {
        RB_EndSurface();
    }

    cmd = (const drawSurfsCommand_t*)data;

    backEnd.refdef = cmd->refdef;
    backEnd.viewParms = cmd->viewParms;
	
	RB_TemporalBeginView();
	RB_SetupFog();
    RB_RenderSpriteSurfList(cmd->drawSurfs, cmd->numDrawSurfs);

    return (const void*)(cmd + 1);
}


/*
=============
RB_DrawBuffer

=============
*/
const void	*RB_DrawBuffer( const void *data ) {
	const drawBufferCommand_t	*cmd;

	cmd = (const drawBufferCommand_t *)data;

	// a new frame (RE_BeginFrame did this before the render thread)
	glState.finishCalled = qfalse;

	// only the back buffer can be drawn to
	vk_begin_frame();

	// clear screen for debugging
	if ( r_clear->integer ) {
		vec4_t clearColor = { 1, 0, 0.5, 1 };
		vk_clear( CLEAR_COLOR | CLEAR_DEPTH, clearColor );
	}

	return (const void *)(cmd + 1);
}

/*
===============
RB_ShowImages

Draw all the images to the screen, on top of whatever
was there.  This is used to test for texture thrashing.

Also called by RE_EndRegistration
===============
*/
void RB_ShowImages( qboolean quiet ) {
	int		i;
	image_t	*image;
	float	x, y, w, h;
	int		start, end;

	if ( !backEnd.in2D ) {
		RB_SetGL2D();
	}

	vk_clear( CLEAR_COLOR, colorBlack );

	vk_finish();

	start = ri.Milliseconds();

	for ( i=0 ; i<tr.numImages ; i++ ) {
		image = &tr.images[i];

		w = glConfig.vidWidth / 20;
		h = glConfig.vidHeight / 15;
		x = i % 20 * w;
		y = i / 20 * h;

		// show in proportional size in mode 2
		if ( r_showImages->integer == 2 ) {
			w *= image->uploadWidth / 512.0f;
			h *= image->uploadHeight / 512.0f;
		}

		R_Bind( image );
		RB_ImmBegin (IMM_QUADS);
		RB_ImmTexCoord2f( 0, 0 );
		RB_ImmVertex2f( x, y );
		RB_ImmTexCoord2f( 1, 0 );
		RB_ImmVertex2f( x + w, y );
		RB_ImmTexCoord2f( 1, 1 );
		RB_ImmVertex2f( x + w, y + h );
		RB_ImmTexCoord2f( 0, 1 );
		RB_ImmVertex2f( x, y + h );
		RB_ImmEnd();
	}

	vk_finish();

	end = ri.Milliseconds();

    if (!quiet) {
        ri.Printf(PRINT_ALL, "%i msec to draw all images\n", end - start);
    }
}

/*
=============
RB_ColorMask

=============
*/
const void *RB_ColorMask(const void *data)
{
	const colorMaskCommand_t *cmd = data;
	
	vk_set_color_mask(cmd->rgba[0], cmd->rgba[1], cmd->rgba[2], cmd->rgba[3]);
	
	return (const void *)(cmd + 1);
}

/*
=============
RB_ClearDepth

=============
*/
const void *RB_ClearDepth(const void *data)
{
	const clearDepthCommand_t *cmd = data;
	
	if(tess.numIndexes)
		RB_EndSurface();

	// texture swapping test
	if (r_showImages->integer)
		RB_ShowImages(qtrue);

	vk_clear(CLEAR_DEPTH, NULL);
	
	return (const void *)(cmd + 1);
}


/*
=============
RB_SwapBuffers

=============
*/
const void	*RB_SwapBuffers( const void *data ) {
	const swapBuffersCommand_t	*cmd;

	// finish any 2D drawing if needed
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	// a scene of the temporal path not ended by 2D drawing goes into the
	// frame before the screenshots
	RB_TemporalEndScene();

	// texture swapping test
	if ( r_showImages->integer ) {
		RB_ShowImages(qfalse);
	}

	// screenshot commands of this frame, now that everything is drawn
	RB_TakePendingScreenshots();

	cmd = (const swapBuffersCommand_t *)data;

	// we measure overdraw by reading back the stencil buffer and
	// counting up the number of increments that have happened: recorded
	// here, summed by the readback writer once the GPU has done it
	// (tr_init.c), nothing waits for it
	vk_probe_input( cmd->inputUs );
	vk_frame_token( cmd->frameToken );

	if ( r_measureOverdraw->integer ) {
		int size[2] = { glConfig.vidWidth, glConfig.vidHeight };

		vk_read_pixels_async( 0, 0, size[0], size[1], READ_STENCIL8, READBACK_OVERDRAW, size, sizeof( size ) );
	}


	// r_finish 1 waits for the GPU once a frame: at the first view, or here
	// in a frame without one. GL1 waited here in every frame without a view
	// (menus, loading screens, the console) to keep the driver from queuing
	// frames; the frame slots' fences bound that already (2 frames in flight)
	if ( r_finish->integer && !glState.finishCalled ) {
		vk_finish();
	}

	vk_end_frame();

	VKimp_EndFrame();

	backEnd.in2D = qfalse;

	return (const void *)(cmd + 1);
}

/*
====================
RB_ExecuteRenderCommands

This function will be called synchronously if running without
smp extensions, or asynchronously by another thread.
====================
*/
void RB_ExecuteRenderCommands( const void *data ) {
	int		t1, t2;

	t1 = ri.Milliseconds ();

	while ( 1 ) {
		int id;

		data = PADP(data, sizeof(void *));
		id = *(const int *)data;

		// r_smp: the 2D draws recorded as commands can leave a batch open
		// between the commands of the list; the commands below always ran
		// with it drawn (R_IssueRenderCommands ended it before the list)
		if ( id != RC_END_OF_LIST && id < RC_END_2D_BATCH ) {
			R_End2DBatch();
		}

		switch ( id ) {
		case RC_SET_COLOR:
			data = RB_SetColor( data );
			break;
		case RC_STRETCH_PIC:
			data = RB_StretchPic( data );
			break;
		case RC_DRAW_SURFS:
			data = RB_DrawSurfs( data );
			break;
		case RC_SPRITE_SURFS:
			data = RB_SpriteSurfs( data );
			break;
		case RC_DRAW_BUFFER:
			data = RB_DrawBuffer( data );
			break;
		case RC_SWAP_BUFFERS:
			data = RB_SwapBuffers( data );
			break;
		case RC_SCREENSHOT:
			data = RB_TakeScreenshotCmd( data );
			break;
		case RC_VIDEOFRAME:
			data = RB_TakeVideoFrameCmd( data );
			break;
		case RC_COLORMASK:
			data = RB_ColorMask(data);
			break;
		case RC_CLEARDEPTH:
			data = RB_ClearDepth(data);
			break;
		case RC_END_2D_BATCH:
		case RC_2D_SET_COLOR:
		case RC_2D_STRETCH_PIC:
		case RC_2D_STRETCH_PIC2:
		case RC_2D_TILE_PIC:
		case RC_2D_TILE_PIC_OFFSET:
		case RC_2D_TRIANGLE_PIC:
		case RC_2D_STRING:
		case RC_2D_ADD_BOX:
		case RC_2D_DRAW_BOX:
		case RC_2D_DRAW_BOXES:
		case RC_2D_LINE_LOOP:
		case RC_2D_WINDOW:
		case RC_2D_SCISSOR:
		case RC_2D_SHADER_TIME:
		case RC_SET_RENDER_TIME:
			data = RB_2DCommand( data );
			break;
		case RC_END_OF_LIST:
		default:
			// stop rendering
			t2 = ri.Milliseconds ();
			backEnd.pc.msec = t2 - t1;
			return;
		}
	}

}
