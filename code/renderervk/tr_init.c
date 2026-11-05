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
// tr_init.c -- functions that are not called every frame

#include "tr_local.h"

#include <SDL3/SDL.h>

glconfig_t	glConfig;
qboolean	textureFilterAnisotropic = qfalse;
int		 maxAnisotropy = 0;
float	   displayAspect = 0;
qboolean	haveClampToEdge;
glstate_t	glState;
int r_sequencenumber;

static void GfxInfo_f( void );

cvar_t	*r_flareSize;
cvar_t	*r_flareFade;

cvar_t	*r_ignoreFastPath;

cvar_t	*r_verbose;
cvar_t	*r_ignore;

cvar_t	*r_displayRefresh;

cvar_t	*r_znear;
cvar_t	*r_depthClamp;

cvar_t	*r_skipBackEnd;

cvar_t	*r_ignorehwgamma;
cvar_t	*r_measureOverdraw;

cvar_t	*r_inGameVideo;
cvar_t	*r_fastsky;
cvar_t	*r_fastdlights;
cvar_t	*r_maxDlights;
cvar_t	*r_drawSun;
cvar_t	*r_dlightBacks;

cvar_t	*r_lodscale;

cvar_t	*r_norefresh;
cvar_t	*r_drawentities;
cvar_t	*r_drawentitypoly;
cvar_t	*r_drawstaticmodels;
cvar_t	*r_drawstaticmodelpoly;
cvar_t	*r_drawbrushes;
cvar_t	*r_drawbrushmodels;
cvar_t	*r_drawstaticdecals;
cvar_t	*r_drawterrain;
cvar_t	*r_drawsprites;
cvar_t	*r_drawspherelights;
cvar_t	*r_drawworld;
cvar_t	*r_speeds;
cvar_t	*r_fullbright;
cvar_t	*r_novis;
cvar_t	*r_nocull;
cvar_t	*r_showcull;
cvar_t	*r_facePlaneCull;
cvar_t	*r_showcluster;
cvar_t	*r_nocurves;

cvar_t	*r_allowExtensions;

cvar_t	*r_ext_compressed_textures;
cvar_t	*r_ext_gamma_control;
cvar_t	*r_ext_multitexture;
cvar_t	*r_ext_compiled_vertex_array;
cvar_t	*r_ext_texture_env_add;

cvar_t	*r_ext_texture_env_combine;
cvar_t	*r_ext_aniso_filter;
cvar_t	*r_ext_max_anisotropy;
cvar_t	*r_forceClampToEdge;
cvar_t	*r_geForce3WorkAround;
cvar_t	*r_reset_tc_array;

cvar_t	*r_logFile;

cvar_t	*r_stencilbits;
cvar_t	*r_depthbits;
cvar_t	*r_colorbits;
cvar_t	*r_stereo;
cvar_t	*r_primitives;
cvar_t	*r_largemap;
cvar_t	*r_textureDetails;
cvar_t	*r_texturebits;

cvar_t	*r_drawBuffer;
cvar_t	*r_lightmap;
cvar_t	*r_vertexLight;
cvar_t	*r_shadows;
cvar_t	*r_entlight_scale;
cvar_t	*r_entlight_errbound;
cvar_t	*r_entlight_cubelevel;
cvar_t	*r_entlight_cubefraction;
cvar_t	*r_entlight_maxcalc;
cvar_t	*r_flares;
cvar_t	*r_mode;
cvar_t	*r_maxmode;
cvar_t	*r_vidmode1024;
cvar_t	*r_vidmodemax;
cvar_t	*r_nobind;
cvar_t	*r_singleShader;
cvar_t	*r_lerpmodels;
cvar_t	*r_roundImagesDown;
cvar_t	*r_colorMipLevels;
cvar_t	*r_picmip;
cvar_t	*r_picmip_cap;
cvar_t	*r_showtris;
cvar_t	*r_showsky;
cvar_t	*r_shownormals;
cvar_t	*r_showhbox;
cvar_t	*r_showstaticbboxes;
cvar_t	*r_finish;
cvar_t	*r_clear;
cvar_t	*r_swapInterval;
cvar_t	*r_textureMode;
cvar_t	*r_offsetFactor;
cvar_t	*r_offsetUnits;
cvar_t	*r_gamma;
cvar_t	*r_intensity;
cvar_t	*r_lockpvs;
cvar_t	*r_noportals;
cvar_t	*r_entlightmap;
cvar_t	*r_fastentlight;
cvar_t	*r_portalOnly;

cvar_t	*r_subdivisions;
cvar_t	*r_lodCurveError;

cvar_t	*r_fullscreen;

cvar_t	*r_customwidth;
cvar_t	*r_customheight;
cvar_t	*r_customaspect;

cvar_t	*r_overBrightBits;
cvar_t	*r_mapOverBrightBits;

cvar_t	*r_debugSurface;

cvar_t	*r_showImages;
cvar_t	*r_showlod;
cvar_t	*r_showstaticlod;

cvar_t	*r_ambientScale;
cvar_t	*r_directedScale;
cvar_t	*r_debugSort;
cvar_t	*r_printShaders;
cvar_t	*r_saveFontData;

cvar_t	*r_maxpolys;
int		max_polys;
cvar_t	*r_maxpolyverts;
int		max_polyverts;
cvar_t* r_maxtermarks;
int		max_termarks;
cvar_t* r_precacheimages;

cvar_t* r_staticlod;
cvar_t* r_lodscale;
cvar_t* r_lodcap;
cvar_t* r_lodviewmodelcap;

cvar_t* r_uselod;
cvar_t* lod_LOD;
cvar_t* lod_minLOD;
cvar_t* lod_maxLOD;
cvar_t* lod_LOD_slider;
cvar_t* lod_curve_0_val;
cvar_t* lod_curve_1_val;
cvar_t* lod_curve_2_val;
cvar_t* lod_curve_3_val;
cvar_t* lod_curve_4_val;
cvar_t* lod_edit_0;
cvar_t* lod_edit_1;
cvar_t* lod_edit_2;
cvar_t* lod_edit_3;
cvar_t* lod_edit_4;
cvar_t* lod_curve_0_slider;
cvar_t* lod_curve_1_slider;
cvar_t* lod_curve_2_slider;
cvar_t* lod_curve_3_slider;
cvar_t* lod_curve_4_slider;
cvar_t* lod_pitch_val;
cvar_t* lod_zee_val;
cvar_t* lod_mesh;
cvar_t* lod_meshname;
cvar_t* lod_tikiname;
cvar_t* lod_metric;
cvar_t* lod_tris;
cvar_t* lod_position;
cvar_t* lod_save;
cvar_t* lod_tool;
cvar_t* r_skelCull;

cvar_t* sys_cpuid;
cvar_t* r_sse;
cvar_t* r_static_shaderdata0;
cvar_t* r_static_shaderdata1;
cvar_t* r_static_shaderdata2;
cvar_t* r_static_shaderdata3;
cvar_t* r_static_shadermultiplier0;
cvar_t* r_static_shadermultiplier1;
cvar_t* r_static_shadermultiplier2;
cvar_t* r_static_shadermultiplier3;

cvar_t* r_numdebuglines;


cvar_t* r_stipplelines;
cvar_t* r_light_lines;
cvar_t* r_light_sun_line;
cvar_t* r_light_int_scale;
cvar_t* r_light_nolight;
cvar_t* r_light_showgrid;
cvar_t* r_skyportal;
cvar_t* r_skyportal_origin;
cvar_t* r_farplane;
cvar_t* r_farplane_bias;
cvar_t* r_farplane_color;
cvar_t* r_farplane_nocull;
cvar_t* r_farplane_nofog;
cvar_t* r_skybox_farplane;
cvar_t* r_farclip;
cvar_t* r_lightcoronasize;
cvar_t* r_useglfog;
cvar_t* r_debuglines_depthmask;
cvar_t* r_smoothsmokelight;
cvar_t* r_showportal;
cvar_t* ter_minMarkRadius;
cvar_t* ter_fastMarks;
cvar_t* r_alpha_foliage1;
cvar_t* r_alpha_foliage2;
cvar_t* r_blendtrees;
cvar_t* r_blendbushes;
cvar_t* r_bumpmap;
cvar_t* r_loadjpg;
cvar_t* r_loadftx;

cvar_t* r_showSkeleton;

cvar_t* r_ext_multisample;
cvar_t* r_noborder;
cvar_t* r_ext_texture_filter_anisotropic;
cvar_t* r_stereoEnabled;
cvar_t* r_anaglyphMode;

cvar_t* r_aviMotionJpegQuality;
cvar_t* r_screenshotJpegQuality;

cvar_t* r_developer;
cvar_t* r_fps;

static char infostring[8192];


/*
** R_ApplyExtensionCvars
**
** Turns off the features the user disabled, after vk_initialize reported
** what the device supports. Same cvars and defaults as the GL1 renderer.
*/
static void R_ApplyExtensionCvars( void )
{
	if ( !r_allowExtensions->integer )
	{
		ri.Printf( PRINT_ALL, "* IGNORING EXTENSIONS *\n" );
		glConfig.textureCompression = TC_NONE;
		glConfig.textureEnvAddAvailable = qfalse;
		glConfig.numTextureUnits = 1;
		textureFilterAnisotropic = qfalse;
		maxAnisotropy = 0;
		return;
	}

	if ( glConfig.textureCompression != TC_NONE && !r_ext_compressed_textures->value )
	{
		ri.Printf( PRINT_ALL, "...ignoring compressed textures\n" );
		glConfig.textureCompression = TC_NONE;
	}

	if ( glConfig.textureEnvAddAvailable && !r_ext_texture_env_add->integer )
	{
		ri.Printf( PRINT_ALL, "...ignoring texture env add\n" );
		glConfig.textureEnvAddAvailable = qfalse;
	}

	if ( glConfig.numTextureUnits > 1 && !r_ext_multitexture->value )
	{
		ri.Printf( PRINT_ALL, "...ignoring multitexture\n" );
		glConfig.numTextureUnits = 1;
	}

	if ( textureFilterAnisotropic && !r_ext_texture_filter_anisotropic->integer )
	{
		ri.Printf( PRINT_ALL, "...ignoring anisotropic filtering\n" );
		textureFilterAnisotropic = qfalse;
		maxAnisotropy = 0;
	}
}

/*
** InitGraphics
**
** This function is responsible for initializing the window and the GPU
** backend, then setting variables and reporting the gfx system config
** to the user.
*/
static void InitGraphics( void )
{
	//
	// initialize OS specific portions of the renderer
	//
	// VKimp_Init directly or indirectly references the following cvars:
	//		- r_fullscreen
	//		- r_mode
	//		- r_ignorehwgamma
	//		- r_gamma
	//
	
	if ( glConfig.vidWidth == 0 )
	{
		VKimp_Init();

		vk_initialize();

		R_ApplyExtensionCvars();

		// stubbed or broken drivers may have reported 0...
		if ( glConfig.maxTextureSize <= 0 ) 
		{
			glConfig.maxTextureSize = 0;
		}

		if (haveClampToEdge) {
			ri.Cvar_Set("r_forceClampToEdge", "1");
		}
	}

	// print info
	GfxInfo_f();

	// set default state
	R_SetDefaultState();
}

/*
** R_GetModeInfo
*/
typedef struct vidmode_s
{
	const char *description;
	int		 width, height;
	float		pixelAspect;		// pixel width / height
} vidmode_t;

vidmode_t r_vidModes[] =
{
	{ "Mode  0: 320x240",		320,	240,	1 },
	{ "Mode  1: 400x300",		400,	300,	1 },
	{ "Mode  2: 512x384",		512,	384,	1 },
	{ "Mode  3: 640x480",		640,	480,	1 },
	{ "Mode  4: 800x600",		800,	600,	1 },
	{ "Mode  5: 960x720",		960,	720,	1 },
	{ "Mode  6: 1024x768",		1024,	768,	1 },
	{ "Mode  7: 1152x864",		1152,	864,	1 },
	{ "Mode  8: 1280x1024",		1280,	1024,	1 },
	{ "Mode  9: 1600x1200",		1600,	1200,	1 },
	{ "Mode 10: 2048x1536",		2048,	1536,	1 },
	{ "Mode 11: 856x480 (wide)",856,	480,	1 }
};
static int	s_numVidModes = ( sizeof( r_vidModes ) / sizeof( r_vidModes[0] ) );

qboolean R_GetModeInfo( int *width, int *height, float *windowAspect, int mode ) {
	vidmode_t	*vm;

	if ( mode < -1 ) {
		return qfalse;
	}
	if ( mode >= s_numVidModes ) {
		return qfalse;
	}

	if ( mode == -1 ) {
		*width = r_customwidth->integer;
		*height = r_customheight->integer;
		*windowAspect = r_customaspect->value;
		return qtrue;
	}

	vm = &r_vidModes[mode];

	*width  = vm->width;
	*height = vm->height;
	*windowAspect = (float)vm->width / ( vm->height * vm->pixelAspect );

	return qtrue;
}

/*
** R_ModeList_f
*/
static void R_ModeList_f( void )
{
	int i;

	ri.Printf( PRINT_ALL, "\n" );
	for ( i = 0; i < s_numVidModes; i++ )
	{
		ri.Printf( PRINT_ALL, "%s\n", r_vidModes[i].description );
	}
	ri.Printf( PRINT_ALL, "\n" );
}


/* 
============================================================================== 
 
						SCREEN SHOTS 

NOTE TTimo
some thoughts about the screenshots system:
screenshots get written in fs_homedatapath + fs_gamedir
vanilla q3 .. baseq3/screenshots/ *.tga
team arena .. missionpack/screenshots/ *.tga

two commands: "screenshot" and "screenshotJPEG"
(here "screenshot" writes PNGs, as "screenshotPNG": see R_ScreenShot_f)
we use statics to store a count and start writing the first screenshot/screenshot????.tga (.jpg) available
(with FS_FileExists / FS_FOpenFileWrite calls)
FIXME: the statics don't get a reinit between fs_game changes

============================================================================== 
*/ 

/*
==================
Pending screenshots

Screenshots, levelshots and resampled screenshots are taken at the end of
the frame, when everything is drawn (RB_SwapBuffers calls
RB_TakePendingScreenshots): the backend draws into the swapchain image,
whose contents are undefined until the frame draws. GL1 read the back
buffer when the command ran, before the frame was drawn. The back end
records the copy there without waiting for the GPU (vk_read_pixels_async)
and the main thread writes the file a few frames later, once the GPU has
done it (R_ProcessReadbacks), with the engine's files and memory on its
own thread. The "Wrote" centerprint of the screenshot command runs once
the file is written, so that it isn't in the picture.
==================
*/
typedef enum {
	SHOT_TGA,
	SHOT_JPEG,
	SHOT_PNG,
	SHOT_LEVELSHOT,
	SHOT_RESAMPLED
} shotType_t;

typedef struct {
	shotType_t	type;
	int			x, y, width, height;
	char		fileName[MAX_OSPATH];
	char		message[MAX_OSPATH + 64];
} pendingShot_t;

#define MAX_PENDING_SHOTS	8

static pendingShot_t	pendingShots[MAX_PENDING_SHOTS];
static int				numPendingShots;

static pendingShot_t *R_AddPendingShot( shotType_t type, const char *fileName ) {
	pendingShot_t *shot;

	if ( numPendingShots == MAX_PENDING_SHOTS ) {
		ri.Printf( PRINT_ALL, "Too many screenshots in one frame, %s skipped\n", fileName ? fileName : "levelshot" );
		return NULL;
	}
	shot = &pendingShots[numPendingShots++];
	Com_Memset( shot, 0, sizeof( *shot ) );
	shot->type = type;
	if ( fileName ) {
		Q_strncpyz( shot->fileName, fileName, sizeof( shot->fileName ) );
	}
	return shot;
}

// on the back end, at the end of the frame: the readbacks of its shots
void RB_TakePendingScreenshots( void ) {
	int i;

	for ( i = 0; i < numPendingShots; i++ ) {
		pendingShot_t *shot = &pendingShots[i];

		if ( shot->type == SHOT_LEVELSHOT || shot->type == SHOT_RESAMPLED ) {
			// sampled down from the whole frame
			vk_read_pixels_async( 0, 0, glConfig.vidWidth, glConfig.vidHeight, READ_RGB8, READBACK_SHOT, shot, sizeof( *shot ) );
		} else {
			vk_read_pixels_async( shot->x, shot->y, shot->width, shot->height, READ_RGB8, READBACK_SHOT, shot, sizeof( *shot ) );
		}
	}
	numPendingShots = 0;
}

/*
==================
RB_TakeScreenshotCmd
==================
*/
const void *RB_TakeScreenshotCmd( const void *data ) {
	const screenshotCommand_t	*cmd;
	pendingShot_t *shot;
	shotType_t type;

	cmd = (const screenshotCommand_t *)data;

	// HDR output: this frame is saved as SDR
	vk_sdr_frame();

	type = (shotType_t)cmd->type;
	shot = R_AddPendingShot( type, cmd->fileName );
	if ( shot ) {
		shot->x = cmd->x;
		shot->y = cmd->y;
		shot->width = cmd->width;
		shot->height = cmd->height;
		Q_strncpyz( shot->message, cmd->message, sizeof( shot->message ) );
	}

	return (const void *)(cmd + 1);
}

/*
==================
R_TakeScreenshot
==================
*/
// the command holds its file name and message (the render thread may run
// it while the next frame's commands are queued)
static void R_QueueShot( shotType_t type, int x, int y, int width, int height, const char *name, const char *message ) {
	screenshotCommand_t	*cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SCREENSHOT;
	cmd->type = type;
	cmd->x = x;
	cmd->y = y;
	cmd->width = width;
	cmd->height = height;
	Q_strncpyz( cmd->fileName, name, sizeof( cmd->fileName ) );
	Q_strncpyz( cmd->message, message ? message : "", sizeof( cmd->message ) );
}

static void R_QueueScreenshot( int x, int y, int width, int height, char *name, qboolean jpeg, qboolean png, const char *message ) {
	R_QueueShot( png ? SHOT_PNG : jpeg ? SHOT_JPEG : SHOT_TGA, x, y, width, height, name, message );
}

void R_TakeScreenshot( int x, int y, int width, int height, char *name, qboolean jpeg, const char *message ) {
	R_QueueScreenshot( x, y, width, height, name, jpeg, qfalse, message );
}

/* 
================== 
R_ScreenshotFilename
================== 
*/  
void R_ScreenshotFilenameJPEG( int lastNumber, char *fileName ) {
	int		a,b,c,d;

	if ( lastNumber < 0 || lastNumber > 9999 ) {
		Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot9999.jpg" );
		return;
	}

	a = lastNumber / 1000;
	lastNumber -= a*1000;
	b = lastNumber / 100;
	lastNumber -= b*100;
	c = lastNumber / 10;
	lastNumber -= c*10;
	d = lastNumber;

	Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot%i%i%i%i.jpg"
		, a, b, c, d );
}

/*
==================
R_ScreenshotFilenamePNG
==================
*/
void R_ScreenshotFilenamePNG( int lastNumber, char *fileName ) {
	int		a,b,c,d;

	if ( lastNumber < 0 || lastNumber > 9999 ) {
		Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot9999.png" );
		return;
	}

	a = lastNumber / 1000;
	lastNumber -= a*1000;
	b = lastNumber / 100;
	lastNumber -= b*100;
	c = lastNumber / 10;
	lastNumber -= c*10;
	d = lastNumber;

	Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot%i%i%i%i.png"
		, a, b, c, d );
}

/*
====================
R_ResampledScreenShot
====================
*/
void R_ResampledScreenShot(const char* filename, int destwidth, int destheight) {
	R_QueueShot( SHOT_RESAMPLED, 0, 0, destwidth, destheight, filename, NULL );
}

/*
====================
R_LevelShot

levelshots are specialized 128*128 thumbnails for
the menu system, sampled down from full screen distorted images
====================
*/
void R_LevelShot( void ) {
	char checkname[MAX_OSPATH];

	if ( !tr.world ) {
		ri.Printf( PRINT_ALL, "levelshot: no map loaded\n" );
		return;
	}
	Com_sprintf( checkname, sizeof( checkname ), "levelshots/%s.tga", tr.world->baseName );
	R_QueueShot( SHOT_LEVELSHOT, 0, 0, 0, 0, checkname, NULL );
}

/* 
================== 
R_ScreenShot_f

screenshot
screenshot [silent]
screenshot [levelshot]
screenshot [filename]
screenshot <path or filename> <width> <height>

Screenshots are PNGs: "screenshot" and "screenshot [silent|filename]" run
screenshotPNG, so old configs that bind "screenshot" get PNGs too. Two
uses stay TGA, what the game reads back: levelshots (levelshots/<map>.tga)
and a shot given a path (with a '/') or a size, the savegame picture that
saveshot takes (<save>.tga at 256x256, shown by the load menu)
================== 
*/  
void R_ScreenShot_f(void) {
	char	checkname[MAX_OSPATH];
	const char	*name;
	int		width, height;

	if (!strcmp(ri.Cmd_Argv(1), "levelshot")) {
		R_LevelShot();
		return;
	}

	if (ri.Cmd_Argc() < 2 || (ri.Cmd_Argc() == 2 && !strchr(ri.Cmd_Argv(1), '/'))) {
		R_ScreenShotPNG_f();
		return;
	}

	name = ri.Cmd_Argv(1);
	if (strchr(name, '/')) {
		Q_strncpyz(checkname, name, sizeof(checkname));
	} else {
		Com_sprintf(checkname, MAX_OSPATH, "screenshots/%s.tga", name);
	}

	width = 0;
	height = 0;
	if (ri.Cmd_Argc() > 2) {
		width = atoi(ri.Cmd_Argv(2));
		height = atoi(ri.Cmd_Argv(3));
	}

	if (!width || !height) {
		char message[MAX_OSPATH + 64];

		Com_sprintf(message, sizeof(message), "centerprint \"%s %s\"\n", ri.LV_ConvertString("Wrote"), checkname);
		R_TakeScreenshot(0, 0, glConfig.vidWidth, glConfig.vidHeight, checkname, qfalse, message);
	} else {
		R_ResampledScreenShot(checkname, width, height);
	}
}

/*
==================
RB_TakeScreenshot
==================
*/
void R_ScreenShotJPEG_f (void) {
	char		checkname[MAX_OSPATH];
	static	int	lastNumber = -1;
	qboolean	silent;

	if ( !strcmp( ri.Cmd_Argv(1), "levelshot" ) ) {
		R_LevelShot();
		return;
	}

	if ( !strcmp( ri.Cmd_Argv(1), "silent" ) ) {
		silent = qtrue;
	} else {
		silent = qfalse;
	}

	if ( ri.Cmd_Argc() == 2 && !silent ) {
		// explicit filename
		Com_sprintf( checkname, MAX_OSPATH, "screenshots/%s.jpg", ri.Cmd_Argv( 1 ) );
	} else {
		// scan for a free filename

		// if we have saved a previous screenshot, don't scan
		// again, because recording demo avis can involve
		// thousands of shots
		if ( lastNumber == -1 ) {
			lastNumber = 0;
		}
		// scan for a free number
		for ( ; lastNumber <= 9999 ; lastNumber++ ) {
			R_ScreenshotFilenameJPEG( lastNumber, checkname );

	  if (!ri.FS_FileExists( checkname ))
	  {
		break; // file doesn't exist
	  }
		}

		if ( lastNumber == 10000 ) {
			ri.Printf (PRINT_ALL, "ScreenShot: Couldn't create a file\n"); 
			return;
 		}

		lastNumber++;
	}

	R_TakeScreenshot( 0, 0, glConfig.vidWidth, glConfig.vidHeight, checkname, qtrue, NULL );

	if ( !silent ) {
		ri.Printf (PRINT_ALL, "Wrote %s\n", checkname);
	}
} 

/*
==================
R_ScreenShotPNG_f
==================
*/
void R_ScreenShotPNG_f (void) {
	char		checkname[MAX_OSPATH];
	static	int	lastNumber = -1;
	qboolean	silent;

	if ( !strcmp( ri.Cmd_Argv(1), "silent" ) ) {
		silent = qtrue;
	} else {
		silent = qfalse;
	}

	if ( ri.Cmd_Argc() == 2 && !silent ) {
		Com_sprintf( checkname, MAX_OSPATH, "screenshots/%s.png", ri.Cmd_Argv( 1 ) );
	} else {
		if ( lastNumber == -1 ) {
			lastNumber = 0;
		}
		for ( ; lastNumber <= 9999 ; lastNumber++ ) {
			R_ScreenshotFilenamePNG( lastNumber, checkname );

			if (!ri.FS_FileExists( checkname ))
			{
				break;
			}
		}

		if ( lastNumber == 10000 ) {
			ri.Printf (PRINT_ALL, "ScreenShot: Couldn't create a file\n");
			return;
		}

		lastNumber++;
	}

	R_QueueScreenshot( 0, 0, glConfig.vidWidth, glConfig.vidHeight, checkname, qfalse, qtrue, NULL );

	if ( !silent ) {
		ri.Printf (PRINT_ALL, "Wrote %s\n", checkname);
	}
}

//============================================================================

/*
==================
RB_TakeVideoFrameCmd

The frame's readback, recorded at the command (the frames of a video are
read when its command runs, as before) without waiting; the main thread
encodes and writes it once the GPU has done it (R_WriteVideoFrame), in its
own buffers: the engine's capture and encode buffers of the command may be
gone by then (stopvideo), and a frame done after that is dropped by
CL_WriteAVIVideoFrame. The last frames recorded before stopvideo are lost
==================
*/
typedef struct {
	int			width, height;
	qboolean	motionJpeg;
} videoReadback_t;

const void *RB_TakeVideoFrameCmd( const void *data )
{
	const videoFrameCommand_t	*cmd;
	videoReadback_t	video;

	// HDR output: the video gets the SDR frame
	vk_sdr_frame();

	cmd = (const videoFrameCommand_t *)data;
	video.width = cmd->width;
	video.height = cmd->height;
	video.motionJpeg = cmd->motionJpeg;
	vk_read_pixels_async( 0, 0, cmd->width, cmd->height, READ_RGB8, READBACK_VIDEO, &video, sizeof( video ) );

	return (const void *)(cmd + 1);
}

/*
==================
The readback writer

The back end's pixel readbacks (vk_read_pixels_async) are taken in the
order they were recorded, on the main thread, once the GPU has done them
(R_ProcessReadbacks). What takes time with them runs on this thread: a
screenshot's conversion, gamma, encoding and writing (about 5 ms for a
2560x1440 TGA, which made a hitch on the main thread), the stencil sum of
r_measureOverdraw and a video frame's encoding. The main thread opens a
shot's file before (ri.FS_OpenFileWrite) and closes it after
(ri.FS_CloseFile, then its message), in order; the thread writes to the
open handle only (ri.FS_Write: an fwrite on it) and allocates with malloc
(libjpeg and stb too). A readback goes back to the pool once its job is
collected; RE_Shutdown waits for every job (R_FlushReadbacks).
==================
*/
#define READBACK_JOBS	8		// as many as the pool's readbacks (vk.c)

typedef struct {
	int				handle;		// the readback
	int				kind;		// READBACK_*
	int				width, height;	// what it read
	int				quality;	// JPEG quality (r_screenshotJpegQuality, r_aviMotionJpegQuality)
	union {
		pendingShot_t	shot;
		videoReadback_t	video;
	} u;
	fileHandle_t	file;		// a shot's, opened by the main thread
	byte			*data;		// a video frame's encoding (malloc)
	size_t			length;
	long			sum;		// the overdraw readback's
} readbackJob_t;

static struct {
	SDL_Thread		*thread;
	SDL_Mutex		*lock;
	SDL_Condition	*wake, *done;
	readbackJob_t	jobs[READBACK_JOBS];	// jobs[n % READBACK_JOBS]
	int				queued, finished, collected;
	qboolean		quit;
} rbw;

// a TGA file of the RGB rows (bottom row first, swapped to BGR); NULL on failure
static byte *R_EncodeTGA( const byte *rgb, int width, int height, size_t *length ) {
	size_t count = (size_t)width * height * 3, i;
	byte *file = malloc( count + 18 );

	if ( !file ) {
		return NULL;
	}
	Com_Memset( file, 0, 18 );
	file[2] = 2;		// uncompressed type
	file[12] = width & 255;
	file[13] = width >> 8;
	file[14] = height & 255;
	file[15] = height >> 8;
	file[16] = 24;		// pixel size
	for ( i = 0; i < count; i += 3 ) {
		file[18 + i] = rgb[i + 2];
		file[18 + i + 1] = rgb[i + 1];
		file[18 + i + 2] = rgb[i];
	}
	*length = count + 18;
	return file;
}

// a TGA file of the RGB rows sampled down to destwidth x destheight (4 x 3
// samples a pixel, a 4:3 grid stretched over the source, as GL1's
// levelshots and resampled screenshots), gamma corrected
static byte *R_EncodeResampledTGA( const byte *rgb, int width, int height, int destwidth, int destheight, size_t *length ) {
	size_t count = (size_t)destwidth * destheight * 3;
	byte *file = malloc( count + 18 );
	float xScale = width / (float)( destwidth * 4 );
	float yScale = height / (float)( destheight * 3 );
	int x, y, xx, yy, r, g, b;

	if ( !file ) {
		return NULL;
	}
	Com_Memset( file, 0, 18 );
	file[2] = 2;
	file[12] = destwidth & 255;
	file[13] = destwidth >> 8;
	file[14] = destheight & 255;
	file[15] = destheight >> 8;
	file[16] = 24;
	for ( y = 0; y < destheight; y++ ) {
		for ( x = 0; x < destwidth; x++ ) {
			byte *dst = file + 18 + 3 * ( y * destwidth + x );

			r = g = b = 0;
			for ( yy = 0; yy < 3; yy++ ) {
				for ( xx = 0; xx < 4; xx++ ) {
					const byte *src = rgb + 3 * width * (int)( ( y * 3 + yy ) * yScale ) + 3 * (int)( ( x * 4 + xx ) * xScale );

					r += src[0];
					g += src[1];
					b += src[2];
				}
			}
			dst[0] = b / 12;
			dst[1] = g / 12;
			dst[2] = r / 12;
		}
	}
	if ( glConfig.deviceSupportsGamma ) {
		R_GammaCorrect( file + 18, count );
	}
	*length = count + 18;
	return file;
}

// on the writer thread: everything but the engine's calls
static void R_RunReadbackJob( readbackJob_t *job ) {
	const int w = job->width, h = job->height;
	const size_t count = (size_t)w * h * 3;
	byte *pixels, *file = NULL;
	size_t length = 0, i;
	int pngLength;

	if ( job->kind == READBACK_OVERDRAW ) {
		pixels = malloc( (size_t)w * h );
		if ( pixels ) {
			vk_readback_get( job->handle, pixels );
			for ( i = 0; i < (size_t)w * h; i++ ) {
				job->sum += pixels[i];
			}
			free( pixels );
		}
		return;
	}
	if ( job->kind == READBACK_SHOT && !job->file ) {
		return;
	}

	pixels = malloc( count );
	if ( !pixels ) {
		return;
	}
	vk_readback_get( job->handle, pixels );

	if ( job->kind == READBACK_VIDEO ) {
		const size_t linelen = (size_t)w * 3;
		const size_t avipadwidth = PAD( linelen, AVI_LINE_PADDING );

		if ( glConfig.deviceSupportsGamma ) {
			R_GammaCorrect( pixels, count );
		}
		job->data = malloc( avipadwidth * h );
		if ( job->data ) {
			if ( job->u.video.motionJpeg ) {
				job->length = RE_SaveJPGToBuffer( job->data, linelen * h, job->quality, w, h, pixels, 0 );
			} else {
				byte *dst = job->data;
				int y;

				// swap R and B and add the AVI line paddings
				for ( y = 0; y < h; y++ ) {
					const byte *src = pixels + linelen * y;

					for ( i = 0; i < linelen; i += 3 ) {
						*dst++ = src[i + 2];
						*dst++ = src[i + 1];
						*dst++ = src[i];
					}
					Com_Memset( dst, 0, avipadwidth - linelen );
					dst += avipadwidth - linelen;
				}
				job->length = avipadwidth * h;
			}
		}
		free( pixels );
		return;
	}

	switch ( job->u.shot.type ) {
	case SHOT_TGA:
		if ( glConfig.deviceSupportsGamma ) {
			R_GammaCorrect( pixels, count );
		}
		file = R_EncodeTGA( pixels, w, h, &length );
		break;
	case SHOT_JPEG:
		if ( glConfig.deviceSupportsGamma ) {
			R_GammaCorrect( pixels, count );
		}
		file = malloc( count );
		if ( file ) {
			length = RE_SaveJPGToBuffer( file, count, job->quality, w, h, pixels, 0 );
		}
		break;
	case SHOT_PNG:
		if ( glConfig.deviceSupportsGamma ) {
			R_GammaCorrect( pixels, count );
		}
		file = RE_EncodePNGFromRGB( w, h, pixels, 0, &pngLength );
		length = file ? (size_t)pngLength : 0;
		break;
	case SHOT_LEVELSHOT:
		file = R_EncodeResampledTGA( pixels, w, h, 128, 128, &length );
		break;
	case SHOT_RESAMPLED:
		file = R_EncodeResampledTGA( pixels, w, h, job->u.shot.width, job->u.shot.height, &length );
		break;
	}
	if ( file && length ) {
		ri.FS_Write( file, length, job->file );
	}
	free( file );
	free( pixels );
}

static int SDLCALL R_ReadbackWriterMain( void *data ) {
	(void)data;

	SDL_LockMutex( rbw.lock );
	for ( ;; ) {
		readbackJob_t *job;

		while ( rbw.finished == rbw.queued && !rbw.quit ) {
			SDL_WaitCondition( rbw.wake, rbw.lock );
		}
		if ( rbw.finished == rbw.queued ) {
			break;	// quit, every job done
		}
		job = &rbw.jobs[rbw.finished % READBACK_JOBS];
		SDL_UnlockMutex( rbw.lock );

		R_RunReadbackJob( job );

		SDL_LockMutex( rbw.lock );
		rbw.finished++;
		SDL_BroadcastCondition( rbw.done );
	}
	SDL_UnlockMutex( rbw.lock );
	return 0;
}

static qboolean R_StartReadbackWriter( void ) {
	if ( rbw.thread ) {
		return qtrue;
	}
	Com_Memset( &rbw, 0, sizeof( rbw ) );
	rbw.lock = SDL_CreateMutex();
	rbw.wake = SDL_CreateCondition();
	rbw.done = SDL_CreateCondition();
	if ( rbw.lock && rbw.wake && rbw.done ) {
		rbw.thread = SDL_CreateThread( R_ReadbackWriterMain, "readbacks", NULL );
	}
	return rbw.thread != NULL;
}

// on the main thread: the jobs the thread finished (with wait, all of them),
// in order: files closed, messages run, sums and video frames passed on,
// the readbacks freed
static void R_CollectReadbackJobs( qboolean wait ) {
	int finished;

	if ( !rbw.lock ) {
		return;
	}
	SDL_LockMutex( rbw.lock );
	while ( wait && rbw.finished < rbw.queued ) {
		SDL_WaitCondition( rbw.done, rbw.lock );
	}
	finished = rbw.finished;
	SDL_UnlockMutex( rbw.lock );

	for ( ; rbw.collected < finished; rbw.collected++ ) {
		readbackJob_t *job = &rbw.jobs[rbw.collected % READBACK_JOBS];

		switch ( job->kind ) {
		case READBACK_SHOT:
			if ( job->file ) {
				ri.FS_CloseFile( job->file );
				if ( job->u.shot.type == SHOT_LEVELSHOT ) {
					ri.Printf( PRINT_ALL, "Wrote %s\n", job->u.shot.fileName );
				}
				if ( job->u.shot.message[0] ) {
					ri.Cmd_ExecuteText( EXEC_APPEND, job->u.shot.message );
				}
			}
			break;
		case READBACK_VIDEO:
			if ( job->data && job->length ) {
				ri.CL_WriteAVIVideoFrame( job->data, (int)job->length );
			}
			free( job->data );
			job->data = NULL;
			break;
		case READBACK_OVERDRAW:
			backEnd.pc.c_overDraw += job->sum;
			break;
		}
		vk_readback_free( job->handle );
	}
}

static void R_StopReadbackWriter( void ) {
	if ( rbw.thread ) {
		R_CollectReadbackJobs( qtrue );
		SDL_LockMutex( rbw.lock );
		rbw.quit = qtrue;
		SDL_SignalCondition( rbw.wake );
		SDL_UnlockMutex( rbw.lock );
		SDL_WaitThread( rbw.thread, NULL );
	}
	if ( rbw.wake ) {
		SDL_DestroyCondition( rbw.wake );
	}
	if ( rbw.done ) {
		SDL_DestroyCondition( rbw.done );
	}
	if ( rbw.lock ) {
		SDL_DestroyMutex( rbw.lock );
	}
	Com_Memset( &rbw, 0, sizeof( rbw ) );
}

/*
==================
R_ProcessReadbacks

On the main thread, at the start of a frame: the jobs the writer finished,
then the readbacks the GPU has done, in the order they were recorded,
handed to the writer (a shot's file opened first)
==================
*/
void R_ProcessReadbacks( void ) {
	int handle, kind, dropped;
	void *user;

	if ( !tr.registered ) {
		return;
	}
	R_CollectReadbackJobs( qfalse );
	dropped = vk_readback_dropped();
	if ( dropped ) {
		ri.Printf( PRINT_WARNING, "%d readbacks dropped (screenshots, video frames): too many in flight\n", dropped );
	}
	while ( rbw.queued - rbw.collected < READBACK_JOBS && ( handle = vk_readback_next( &kind, &user ) ) != 0 ) {
		readbackJob_t *job;

		if ( !R_StartReadbackWriter() ) {
			ri.Printf( PRINT_WARNING, "no readback writer thread (%s): readback dropped\n", SDL_GetError() );
			vk_readback_free( handle );
			continue;
		}
		job = &rbw.jobs[rbw.queued % READBACK_JOBS];
		Com_Memset( job, 0, sizeof( *job ) );
		job->handle = handle;
		job->kind = kind;
		switch ( kind ) {
		case READBACK_SHOT:
			job->u.shot = *(const pendingShot_t *)user;
			if ( job->u.shot.type == SHOT_LEVELSHOT || job->u.shot.type == SHOT_RESAMPLED ) {
				// read whole (RB_TakePendingScreenshots)
				job->width = glConfig.vidWidth;
				job->height = glConfig.vidHeight;
			} else {
				job->width = job->u.shot.width;
				job->height = job->u.shot.height;
			}
			job->quality = r_screenshotJpegQuality->integer;
			job->file = ri.FS_OpenFileWrite( job->u.shot.fileName );
			break;
		case READBACK_VIDEO:
			job->u.video = *(const videoReadback_t *)user;
			job->width = job->u.video.width;
			job->height = job->u.video.height;
			job->quality = r_aviMotionJpegQuality->integer;
			break;
		case READBACK_OVERDRAW:
			job->width = ( (const int *)user )[0];
			job->height = ( (const int *)user )[1];
			break;
		}
		SDL_LockMutex( rbw.lock );
		rbw.queued++;
		SDL_SignalCondition( rbw.wake );
		SDL_UnlockMutex( rbw.lock );
	}
}

// before the renderer shuts down: every readback of the frames submitted
// is done and written (a screenshot right before quitting is saved), and
// the writer stops
void R_FlushReadbacks( void ) {
	if ( !tr.registered ) {
		return;
	}
	vk_finish();
	R_ProcessReadbacks();
	R_StopReadbackWriter();
}

//============================================================================

/*
** R_SetDefaultState
*/
void R_SetDefaultState( void )
{
	float identity[16];

	vk_set_cull( CULL_NONE );

	RB_SetColor4f (1,1,1,1);

	// texture env values the backend starts from
	glState.texEnv[0] = glState.texEnv[1] = TEXENV_MODULATE;

	// initialize downstream texture unit if we're running
	// in a multitexture environment
	if ( glConfig.numTextureUnits > 1 ) {
		R_SelectTexture( 1 );
		R_TextureMode( r_textureMode->string );
		vk_set_texenv( 1, TEXENV_MODULATE );
		R_EnableTexture( qfalse );
		R_SelectTexture( 0 );
	}

	R_EnableTexture( qtrue );
	R_TextureMode( r_textureMode->string );
	vk_set_texenv( 0, TEXENV_MODULATE );

	//
	// make sure our state vector is set correctly
	//
	glState.glStateBits = GLS_DEPTHTEST_DISABLE | GLS_DEPTHMASK_TRUE;
	vk_set_state( glState.glStateBits );

	// the rest is the initial state the renderer relies on
	Com_Memset( &glState.arrays, 0, sizeof( glState.arrays ) );
	RB_LoadIdentity( identity );
	RB_SetProjection( identity );
	RB_SetModelview( identity );
	RB_SetViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	RB_SetScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	vk_set_depth_range( 0.0f, 1.0f );
	vk_set_color_mask( qtrue, qtrue, qtrue, qtrue );
	vk_set_polygon_offset( qfalse, 0, 0 );
	vk_set_clip_plane( NULL );
	vk_set_stencil_test( qfalse );
	vk_set_stencil_func( STENCILFUNC_ALWAYS, 0, ~0U );
	vk_set_stencil_op( STENCILOP_KEEP, STENCILOP_KEEP, STENCILOP_KEEP );
	vk_set_stencil_write_mask( ~0U );
	vk_set_line_width( 1.0f );
	vk_set_line_stipple( 0, 0xFFFF );

	vk_set_fog( qfalse );
	RB_SetFogRange( 0.0f, 1.0f );

	glState.fFogColor[0] = 0.0;
	glState.fFogColor[1] = 0.0;
	glState.fFogColor[2] = 0.0;
	glState.fFogColor[3] = 1.0;
	vk_set_fog_color( glState.fFogColor );
}

/*
==================
PrintAndAppendString
==================
*/
void PrintAndAppendString(char* string, size_t maxlen, char* fmt, ...) {
	char msg[1024];
	size_t len;
	va_list va;

	va_start(va, fmt);
	Q_vsnprintf(msg, sizeof(msg), fmt, va);
	va_end(va);

	ri.Printf(PRINT_ALL, "%s", msg);

	Q_strcat(string, maxlen, msg);
}

/*
==================
RE_GetGraphicsInfo
==================
*/
const char* RE_GetGraphicsInfo() {
	return infostring;
}

/*
================
R_PrintLongString

Workaround for ri.Printf's 1024 characters buffer limit.
================
*/
void R_PrintLongString(char* dest, size_t destsize, const char *string) {
	char buffer[1024];
	const char *p;
	int size = strlen(string);

	p = string;
	while(size > 0)
	{
		Q_strncpyz(buffer, p, sizeof (buffer) );
		PrintAndAppendString( dest, destsize, "%s", buffer );
		p += 1023;
		size -= 1023;
	}
}

/*
==================
BuildGfxInfo
==================
*/
void BuildGfxInfo(char* dest, size_t destsize) {
	cvar_t *sys_cpustring = ri.Cvar_Get( "sys_cpustring", "", 0 );
	const char *enablestrings[] =
	{
		"disabled",
		"enabled"
	};
	const char *fsstrings[] =
	{
		"windowed",
		"fullscreen"
	};

	PrintAndAppendString( dest, destsize, "\nVENDOR: %s\n", glConfig.vendor_string );
	PrintAndAppendString( dest, destsize, "RENDERER: %s\n", glConfig.renderer_string );
	PrintAndAppendString( dest, destsize, "VERSION: %s\n", glConfig.version_string );
	PrintAndAppendString( dest, destsize, "EXTENSIONS: " );
	R_PrintLongString( dest, destsize, glConfig.extensions_string );
	PrintAndAppendString( dest, destsize, "\n" );
	PrintAndAppendString( dest, destsize, "MAX_TEXTURE_SIZE: %d\n", glConfig.maxTextureSize );
	PrintAndAppendString( dest, destsize, "MAX_TEXTURE_UNITS: %d\n", glConfig.numTextureUnits );
	PrintAndAppendString( dest, destsize, "\nPIXELFORMAT: color(%d-bits) Z(%d-bit) stencil(%d-bits)\n", glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits );
	PrintAndAppendString( dest, destsize, "MODE: %d, %d x %d %s hz:", r_mode->integer, glConfig.vidWidth, glConfig.vidHeight, fsstrings[r_fullscreen->integer == 1] );
	if ( glConfig.displayFrequency )
	{
		PrintAndAppendString( dest, destsize, "%d\n", glConfig.displayFrequency );
	}
	else
	{
		PrintAndAppendString( dest, destsize, "N/A\n" );
	}
	if ( glConfig.deviceSupportsGamma )
	{
		PrintAndAppendString( dest, destsize, "GAMMA: hardware w/ %d overbright bits\n", tr.overbrightBits );
	}
	else
	{
		PrintAndAppendString( dest, destsize, "GAMMA: software w/ %d overbright bits\n", tr.overbrightBits );
	}
	PrintAndAppendString( dest, destsize, "CPU: %s\n", sys_cpustring->string );

	// rendering primitives
	PrintAndAppendString( dest, destsize, "rendering primitives: " );
	if ( r_primitives->integer < 0 || r_primitives->integer > 3 ) {
		PrintAndAppendString( dest, destsize, "none\n" );
	} else {
		PrintAndAppendString( dest, destsize, "indexed triangles\n" );
	}

	PrintAndAppendString( dest, destsize, "texturemode: %s\n", r_textureMode->string );
	PrintAndAppendString( dest, destsize, "picmip: %d\n", r_picmip->integer );
	PrintAndAppendString( dest, destsize, "texture bits: %d\n", r_texturebits->integer );
	PrintAndAppendString( dest, destsize, "multitexture: %s\n", enablestrings[glConfig.numTextureUnits > 1] );
	PrintAndAppendString( dest, destsize, "texenv add: %s\n", enablestrings[glConfig.textureEnvAddAvailable != 0] );
	PrintAndAppendString( dest, destsize, "compressed textures: %s\n", enablestrings[glConfig.textureCompression!=TC_NONE] );
	if ( r_vertexLight->integer || glConfig.hardwareType == GLHW_PERMEDIA2 )
	{
		PrintAndAppendString( dest, destsize, "HACK: using vertex lightmap approximation\n" );
	}
	if ( glConfig.hardwareType == GLHW_RAGEPRO )
	{
		PrintAndAppendString( dest, destsize, "HACK: ragePro approximations\n" );
	}
	if ( glConfig.hardwareType == GLHW_RIVA128 )
	{
		PrintAndAppendString( dest, destsize, "HACK: riva128 approximations\n" );
	}
	if ( glConfig.smpActive ) {
		PrintAndAppendString( dest, destsize, "Using dual processor acceleration\n" );
	}
	if ( r_finish->integer ) {
		PrintAndAppendString( dest, destsize, "Forcing GPU sync every frame\n" );
	}
}

/*
================
GfxInfo_f
================
*/
void GfxInfo_f( void ) 
{
	BuildGfxInfo(infostring, sizeof(infostring));
	ri.Cvar_Set("r_gfxinfo", infostring);
}

/*
==================
FarPlaneInfo_f
==================
*/
void FarPlaneInfo_f(void) {
	ri.Printf(PRINT_ALL, "Current fog settings:\n");
	ri.Printf(PRINT_ALL, "Distance: %i\n", (int)tr.viewParms.farplane_distance);
	ri.Printf(PRINT_ALL, 
		"Color: %f  %f  %f\n",
		tr.viewParms.farplane_color[0],
		tr.viewParms.farplane_color[1],
		tr.viewParms.farplane_color[2]);
	if (tr.viewParms.farplane_cull) {
		ri.Printf(PRINT_ALL, "Cull: on\n");
	} else {
		ri.Printf(PRINT_ALL, "Cull: off\n");
	}
}

/*
==================
R_SetMode
==================
*/
qboolean R_SetMode(int mode, const glconfig_t* glConfig) {
    // FIXME: unimplemented
	ri.Printf(PRINT_WARNING, "R_SetMode is unimplemented. To change the video mode, set the \"r_mode\" variable and execute the \"vid_restart\" command.\n");
	return qfalse;
}

/*
==================
R_SetFullscreen
==================
*/
void R_SetFullscreen(qboolean fullscreen) {
    // FIXME: unimplemented
    ri.Printf(PRINT_WARNING, "R_SetFullscreen is unimplemented. To change the fullscreen mode, set the \"r_fullscreen\" variable and execute the \"vid_restart\" command.\n");
}

/*
===============
R_Register
===============
*/
void R_Register( void ) 
{
	//
	// latched and archived variables
	//
	r_allowExtensions = ri.Cvar_Get( "r_allowExtensions", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_compressed_textures = ri.Cvar_Get( "r_ext_compressed_textures", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_gamma_control = ri.Cvar_Get( "r_ext_gamma_control", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_multitexture = ri.Cvar_Get( "r_ext_multitexture", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_compiled_vertex_array = ri.Cvar_Get( "r_ext_compiled_vertex_array", "1", CVAR_ARCHIVE | CVAR_LATCH);
#ifdef __linux__ // broken on linux
	r_ext_texture_env_add = ri.Cvar_Get( "r_ext_texture_env_add", "0", CVAR_ARCHIVE | CVAR_LATCH);
#else
	r_ext_texture_env_add = ri.Cvar_Get( "r_ext_texture_env_add", "1", CVAR_ARCHIVE | CVAR_LATCH);
#endif
	r_ext_texture_env_combine = ri.Cvar_Get("r_ext_texture_env_combine", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_ext_aniso_filter = ri.Cvar_Get("r_ext_aniso_filter", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_ext_max_anisotropy = ri.Cvar_Get("r_ext_max_anisotropy", "2", CVAR_ARCHIVE | CVAR_LATCH);
	r_forceClampToEdge = ri.Cvar_Get("r_forceClampToEdge", "0", CVAR_ROM);
	r_geForce3WorkAround = ri.Cvar_Get("r_geForce3WorkAround", "1", CVAR_ARCHIVE);
	r_reset_tc_array = ri.Cvar_Get("r_reset_tc_array", "1", CVAR_ARCHIVE);

	r_picmip = ri.Cvar_Get ("r_picmip", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_picmip_cap = ri.Cvar_Get ("r_picmip_cap", "0", CVAR_ARCHIVE | CVAR_LATCH );
    if (r_picmip->integer < r_picmip_cap->integer) {
        ri.Cvar_Set("r_picmip", r_picmip_cap->string);
    }

    if (r_picmip->integer < 2) {
        r_largemap = ri.Cvar_Get("r_largemap", "1", 0);
	} else {
		r_largemap = ri.Cvar_Get("r_largemap", "0", 0);
	}
	
	r_roundImagesDown = ri.Cvar_Get ("r_roundImagesDown", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_colorMipLevels = ri.Cvar_Get ("r_colorMipLevels", "0", CVAR_LATCH );
	ri.Cvar_CheckRange( r_picmip, 0, 16, qtrue );
	r_textureDetails = ri.Cvar_Get("r_textureDetails", "1", 33);
	r_texturebits = ri.Cvar_Get( "r_texturebits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_colorbits = ri.Cvar_Get( "r_colorbits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_stereo = ri.Cvar_Get( "r_stereo", "0", CVAR_ARCHIVE | CVAR_LATCH );
#ifdef __linux__
	r_stencilbits = ri.Cvar_Get( "r_stencilbits", "0", CVAR_ARCHIVE | CVAR_LATCH );
#else
	r_stencilbits = ri.Cvar_Get( "r_stencilbits", "8", CVAR_ARCHIVE | CVAR_LATCH );
#endif
	r_depthbits = ri.Cvar_Get("r_depthbits", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_overBrightBits = ri.Cvar_Get ("r_overBrightBits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_ignorehwgamma = ri.Cvar_Get( "r_ignorehwgamma", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_maxmode = ri.Cvar_Get("r_maxmode", "6", CVAR_ARCHIVE | CVAR_LATCH);
	r_vidmode1024 = ri.Cvar_Get("r_vidmode1024", "1", CVAR_ARCHIVE | CVAR_LATCH);
	r_vidmodemax = ri.Cvar_Get("r_vidmodemax", "1", CVAR_ARCHIVE | CVAR_LATCH);
	r_mode = ri.Cvar_Get("r_mode", "-2", CVAR_ARCHIVE | CVAR_LATCH);
	
	// Added in 2.0
	//  But removed because it's kind of annoying
	//if (r_mode->integer > r_maxmode->integer)
	//{
	//	char buf[8];
	//
	//	Com_sprintf(buf, sizeof(buf), "%d", r_maxmode->integer);
	//	ri.Cvar_Set("r_mode", buf);
	//	r_mode = ri.Cvar_Get("r_mode", "3", CVAR_ARCHIVE | CVAR_LATCH);
	//}
	r_fullscreen = ri.Cvar_Get("r_fullscreen", "1", CVAR_ARCHIVE | CVAR_LATCH);
	r_customwidth = ri.Cvar_Get( "r_customwidth", "1600", CVAR_ARCHIVE | CVAR_LATCH );
	r_customheight = ri.Cvar_Get( "r_customheight", "1024", CVAR_ARCHIVE | CVAR_LATCH );
	r_customaspect = ri.Cvar_Get( "r_customaspect", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_vertexLight = ri.Cvar_Get("r_vertexLight", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_ignoreFastPath = ri.Cvar_Get("r_ignoreFastPath", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_subdivisions = ri.Cvar_Get("r_subdivisions", "3", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_CheckRange(r_subdivisions, 2, 24, qtrue);

	//
	// temporary latched variables that can only change over a restart
	//
	r_displayRefresh = ri.Cvar_Get( "r_displayRefresh", "0", CVAR_LATCH );
	ri.Cvar_CheckRange( r_displayRefresh, 0, 100000, qtrue );
	r_fullbright = ri.Cvar_Get ("r_fullbright", "0", CVAR_LATCH|CVAR_CHEAT );
	r_mapOverBrightBits = ri.Cvar_Get ("r_mapOverBrightBits", "1", CVAR_LATCH );
	r_intensity = ri.Cvar_Get ("r_intensity", "1", CVAR_LATCH );
	r_numdebuglines = ri.Cvar_Get("g_numdebuglines", "4096", CVAR_LATCH);
	r_singleShader = ri.Cvar_Get ("r_singleShader", "0", CVAR_CHEAT | CVAR_LATCH );

	//
	// archived variables that can change at any time
	//
	r_lerpmodels = ri.Cvar_Get("r_lerpmodels", "1", 0);
	r_lodCurveError = ri.Cvar_Get( "r_lodCurveError", "250", CVAR_ARCHIVE|CVAR_CHEAT );
	r_flares = ri.Cvar_Get ("r_flares", "0", CVAR_ARCHIVE );
	r_znear = ri.Cvar_Get( "r_znear", "4", CVAR_CHEAT );
	ri.Cvar_CheckRange( r_znear, 0.001f, 200, qtrue );
	r_depthClamp = ri.Cvar_Get( "r_depthClamp", "2", CVAR_CHEAT );
	ri.Cvar_CheckRange( r_depthClamp, 0, 2, qtrue );
	r_fastsky = ri.Cvar_Get( "r_fastsky", "0", 0 );
	r_fastdlights = ri.Cvar_Get("r_fastdlights", "0", CVAR_ARCHIVE | CVAR_LATCH);
	// the dynamic lights a scene keeps, the nearest the view (R_KeepNearestDlights)
	r_maxDlights = ri.Cvar_Get( "r_maxDlights", "4", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_CheckRange( r_maxDlights, 0, MAX_SCENE_DLIGHTS, qtrue );
	r_drawSun = ri.Cvar_Get( "r_drawSun", "0", CVAR_ARCHIVE );
	r_dlightBacks = ri.Cvar_Get( "r_dlightBacks", "1", CVAR_ARCHIVE );
	r_finish = ri.Cvar_Get ("r_finish", "0", CVAR_ARCHIVE);
	r_textureMode = ri.Cvar_Get( "r_textureMode", "gl_linear_mipmap_linear", CVAR_ARCHIVE );
	r_swapInterval = ri.Cvar_Get( "r_swapInterval", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_gamma = ri.Cvar_Get( "r_gamma", "1", CVAR_ARCHIVE );
	r_facePlaneCull = ri.Cvar_Get ("r_facePlaneCull", "1", CVAR_ARCHIVE );
	//	Added in OPM
	//	 Make archivable
	r_drawstaticdecals = ri.Cvar_Get("r_drawstaticdecals", "1", CVAR_ARCHIVE );

	r_primitives = ri.Cvar_Get( "r_primitives", "0", CVAR_ARCHIVE );

	r_ambientScale = ri.Cvar_Get( "r_ambientScale", "0.6", CVAR_CHEAT );
	r_directedScale = ri.Cvar_Get( "r_directedScale", "1", CVAR_CHEAT );

	r_anaglyphMode = ri.Cvar_Get("r_anaglyphMode", "0", CVAR_ARCHIVE);

	//
	// temporary variables that can change at any time
	//
	r_showImages = ri.Cvar_Get( "r_showImages", "0", CVAR_TEMP );
	r_showlod = ri.Cvar_Get("r_showlod", "0", CVAR_TEMP);
	r_showstaticlod = ri.Cvar_Get("r_showstaticlod", "0", CVAR_TEMP);
	// skeletal models at full detail at any distance unless r_uselod 1 (MoHAA Reloaded's default, upstream's is 1)
	r_uselod = ri.Cvar_Get("r_uselod", "0", CVAR_TEMP);
	lod_LOD = ri.Cvar_Get("lod_LOD", "0", CVAR_TEMP);
	lod_minLOD = ri.Cvar_Get("lod_minLOD", "1.0", CVAR_TEMP);
	lod_maxLOD = ri.Cvar_Get("lod_maxLOD", "0.3", CVAR_TEMP);
	lod_LOD_slider = ri.Cvar_Get("lod_LOD_slider", "0.5", CVAR_TEMP);
	lod_edit_0 = ri.Cvar_Get("lod_edit_0", "0", CVAR_TEMP);
	lod_edit_1 = ri.Cvar_Get("lod_edit_1", "0", CVAR_TEMP);
	lod_edit_2 = ri.Cvar_Get("lod_edit_2", "0", CVAR_TEMP);
	lod_edit_3 = ri.Cvar_Get("lod_edit_3", "0", CVAR_TEMP);
	lod_edit_4 = ri.Cvar_Get("lod_edit_4", "0", CVAR_TEMP);
	lod_curve_0_val = ri.Cvar_Get("lod_curve_0_val", "0", CVAR_TEMP);
	lod_curve_1_val = ri.Cvar_Get("lod_curve_1_val", "0", CVAR_TEMP);
	lod_curve_2_val = ri.Cvar_Get("lod_curve_2_val", "0", CVAR_TEMP);
	lod_curve_3_val = ri.Cvar_Get("lod_curve_3_val", "0", CVAR_TEMP);
	lod_curve_4_val = ri.Cvar_Get("lod_curve_4_val", "0", CVAR_TEMP);
	lod_curve_0_slider = ri.Cvar_Get("lod_curve_0_slider", "0", CVAR_TEMP);
	lod_curve_1_slider = ri.Cvar_Get("lod_curve_1_slider", "0", CVAR_TEMP);
	lod_curve_2_slider = ri.Cvar_Get("lod_curve_2_slider", "0", CVAR_TEMP);
	lod_curve_3_slider = ri.Cvar_Get("lod_curve_3_slider", "0", CVAR_TEMP);
	lod_curve_4_slider = ri.Cvar_Get("lod_curve_4_slider", "0", CVAR_TEMP);
	lod_pitch_val = ri.Cvar_Get("lod_pitch_val", "0", CVAR_TEMP);
	lod_zee_val = ri.Cvar_Get("lod_zee_val", "0", CVAR_TEMP);
	lod_mesh = ri.Cvar_Get("lod_mesh", "0", CVAR_TEMP);
	lod_meshname = ri.Cvar_Get("lod_meshname", "", CVAR_TEMP);
	lod_tikiname = ri.Cvar_Get("lod_tikiname", "", CVAR_TEMP);
	lod_metric = ri.Cvar_Get("lod_metric", "0.0", CVAR_TEMP);
	lod_tris = ri.Cvar_Get("lod_tris", "", CVAR_TEMP);
	lod_save = ri.Cvar_Get("lod_save", "0", CVAR_TEMP);
	lod_position = ri.Cvar_Get("lod_position", "0 0 0", CVAR_TEMP);
	lod_tool = ri.Cvar_Get("lod_tool", "0", CVAR_TEMP);

	r_debugSort = ri.Cvar_Get( "r_debugSort", "0", CVAR_CHEAT );
	r_printShaders = ri.Cvar_Get( "r_printShaders", "0", 0 );

	r_nocurves = ri.Cvar_Get ("r_nocurves", "0", CVAR_CHEAT );
	r_drawworld = ri.Cvar_Get ("r_drawworld", "1", CVAR_CHEAT );
	r_lightmap = ri.Cvar_Get ("r_lightmap", "0", 0 );
	r_portalOnly = ri.Cvar_Get ("r_portalOnly", "0", CVAR_CHEAT );

	r_flareSize = ri.Cvar_Get ("r_flareSize", "40", CVAR_CHEAT);
	r_flareFade = ri.Cvar_Get ("r_flareFade", "7", CVAR_CHEAT);

	r_skipBackEnd = ri.Cvar_Get ("r_skipBackEnd", "0", CVAR_CHEAT);

	r_measureOverdraw = ri.Cvar_Get( "r_measureOverdraw", "0", CVAR_CHEAT );
	r_norefresh = ri.Cvar_Get("r_norefresh", "0", CVAR_CHEAT);
	r_drawentities = ri.Cvar_Get("r_drawentities", "1", CVAR_CHEAT);
	r_drawentitypoly = ri.Cvar_Get("r_drawentitypoly", "1", CVAR_CHEAT);
	r_drawstaticmodels = ri.Cvar_Get("r_drawstaticmodels", "1", CVAR_CHEAT);
	r_drawstaticmodelpoly = ri.Cvar_Get("r_drawstaticmodelpoly", "1", CVAR_CHEAT);
	// 0 tessellates world surfaces and static models on the CPU (tr_staticgeom.c)
	r_staticGeometry = ri.Cvar_Get("r_staticGeometry", "1", 0);
	// 0 leaves the static batches whose shaders compute something per
	// vertex (deforms, tcGen, tcMod, distFade, static models' dynamic
	// lights) to the CPU (shaders/effects.vert)
	r_staticEffects = ri.Cvar_Get("r_staticEffects", "1", 0);
	// 0 copies the terrain's vertexes into tess every frame instead of drawing
	// them from the terrain pool (tr_terrain.c, vk_sync_terrain_pool)
	r_staticTerrain = ri.Cvar_Get("r_staticTerrain", "1", 0);
	// what the terrain's tessellation leaves out (tr_terrain.c): 1 the
	// geomorph of patches whose vertexes need no new height, 2 the merging
	// in patches where nothing changed since it last failed, 4 the walk for
	// a patch's indexes while its triangles stay (RB_DrawTerrainTris); 0
	// walks everything every frame
	r_terrainCache = ri.Cvar_Get("r_terrainCache", "7", 0);
	// 0 skins and lights skeletal models on the CPU (tr_skin.c)
	r_gpuSkinning = ri.Cvar_Get("r_gpuSkinning", "1", 0);
	// 0 prepares and draws skeletal models outside the view too (tr_model.cpp)
	r_skelCull = ri.Cvar_Get("r_skelCull", "1", 0);
	// 1: consecutive 2D draws of one shader are one surface, 2: DrawBox leaves
	// out the boxes that change no pixel; 0 draws each as it comes (tr_draw.c)
	r_batch2D = ri.Cvar_Get("r_batch2D", "3", 0);
	// the opaque shaders drawn in the order of their pipelines (tr_shader.c,
	// SortNewShader), so the back end binds fewer
	r_sortPipelines = ri.Cvar_Get("r_sortPipelines", "1", CVAR_LATCH);
	ri.Cvar_SetDescription(r_sortPipelines, "1: the opaque shaders are drawn in the order of the pipelines their draws bind (fewer pipeline binds a frame); 0: in the order they were loaded, as before. Opaque draws with the same depth at a pixel (z-fighting) can come out the other way");
	// the back end on a render thread (tr_smp.c)
	r_smp = ri.Cvar_Get("r_smp", "1", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_smp, "1: the back end (the draws' recording, the submission and the present) runs on a render thread while the game and the front end build the next frame; the frame rate is the slower half's, a frame's input to its present as before (see README, Render thread); 0: one thread");
	r_drawbrushes = ri.Cvar_Get("r_drawbrushes", "1", CVAR_CHEAT);
	r_drawbrushmodels = ri.Cvar_Get("r_drawbrushmodels", "1", CVAR_CHEAT);	
	r_drawterrain = ri.Cvar_Get("r_drawterrain", "1", CVAR_CHEAT);
	r_drawsprites = ri.Cvar_Get("r_drawsprites", "1", CVAR_CHEAT);
	r_drawspherelights = ri.Cvar_Get("r_drawspherelights", "1", CVAR_CHEAT);
	r_staticlod = ri.Cvar_Get("r_staticlod", "1", CVAR_CHEAT);
	r_lodscale = ri.Cvar_Get("r_lodscale", "1.1", CVAR_ARCHIVE);
	r_lodcap = ri.Cvar_Get("r_lodcap", "1.0", CVAR_ARCHIVE);
	r_lodviewmodelcap = ri.Cvar_Get("r_lodviewmodelcap", "0.25", CVAR_ARCHIVE);
	r_ignore = ri.Cvar_Get( "r_ignore", "1", CVAR_CHEAT );
	r_nocull = ri.Cvar_Get ("r_nocull", "0", CVAR_CHEAT);
	r_showcull = ri.Cvar_Get("r_showcull", "0", CVAR_CHEAT);
	r_novis = ri.Cvar_Get ("r_novis", "0", CVAR_CHEAT);
	r_showcluster = ri.Cvar_Get ("r_showcluster", "0", CVAR_CHEAT);
	r_speeds = ri.Cvar_Get ("r_speeds", "0", CVAR_CHEAT);
	r_verbose = ri.Cvar_Get( "r_verbose", "0", CVAR_CHEAT );
	r_logFile = ri.Cvar_Get( "r_logFile", "0", CVAR_CHEAT );
	r_debugSurface = ri.Cvar_Get ("r_debugSurface", "0", CVAR_CHEAT);
	r_nobind = ri.Cvar_Get ("r_nobind", "0", CVAR_CHEAT);
	r_showtris = ri.Cvar_Get ("r_showtris", "0", CVAR_CHEAT);
	r_showsky = ri.Cvar_Get ("r_showsky", "0", CVAR_CHEAT);
	r_shownormals = ri.Cvar_Get ("r_shownormals", "0", CVAR_CHEAT);
	r_showhbox = ri.Cvar_Get("r_showhbox", "0", CVAR_CHEAT);
	r_showstaticbboxes = ri.Cvar_Get("r_showstaticbboxes", "0", CVAR_CHEAT);
	r_clear = ri.Cvar_Get ("r_clear", "0", CVAR_CHEAT);
	r_offsetFactor = ri.Cvar_Get( "r_offsetfactor", "-1", CVAR_CHEAT );
	r_offsetUnits = ri.Cvar_Get( "r_offsetunits", "-2", CVAR_CHEAT );
	r_drawBuffer = ri.Cvar_Get( "r_drawBuffer", "GL_BACK", CVAR_CHEAT );
	r_lockpvs = ri.Cvar_Get ("r_lockpvs", "0", CVAR_CHEAT);
	r_noportals = ri.Cvar_Get ("r_noportals", "0", CVAR_CHEAT);
	r_entlightmap = ri.Cvar_Get("r_entlightmap", "0", CVAR_CHEAT);
	r_fastentlight = ri.Cvar_Get("r_fastentlight", "0", CVAR_ARCHIVE);
	r_entlight_scale = ri.Cvar_Get("r_entlight_scale", "1.3", CVAR_CHEAT);
	r_entlight_errbound = ri.Cvar_Get("r_entlight_errbound", "6", CVAR_ARCHIVE);
	r_entlight_cubelevel = ri.Cvar_Get("r_entlight_cubelevel", "0", CVAR_ARCHIVE);
	r_entlight_cubefraction = ri.Cvar_Get("r_entlight_cubefraction", "0.5", CVAR_ARCHIVE);
	r_entlight_maxcalc = ri.Cvar_Get("r_entlight_maxcalc", "2", CVAR_ARCHIVE);
	r_shadows = ri.Cvar_Get( "cg_shadows", "1", 0 );

	r_maxpolys = ri.Cvar_Get( "r_maxpolys", va("%d", MAX_POLYS), 0);
	r_maxpolyverts = ri.Cvar_Get( "r_maxpolyverts", va("%d", MAX_POLYVERTS), 0);
	r_maxtermarks = ri.Cvar_Get("r_maxtermarks", va("%d", MAX_TERMARKS), 0);

	r_stipplelines = ri.Cvar_Get("r_stipplelines", "1", CVAR_ARCHIVE);
	r_light_lines = ri.Cvar_Get("r_light_lines", "0", CVAR_CHEAT);
	r_light_sun_line = ri.Cvar_Get("r_light_sun_line", "0", CVAR_CHEAT);
	r_light_int_scale = ri.Cvar_Get("r_light_int_scale", "0.05", CVAR_ARCHIVE);
	r_light_nolight = ri.Cvar_Get("r_light_nolight", "0", CVAR_CHEAT | CVAR_ARCHIVE);
	r_light_showgrid = ri.Cvar_Get("r_light_showgrid", "0", CVAR_CHEAT);
	r_skyportal = ri.Cvar_Get("r_skyportal", "0", 0);
	r_skyportal_origin = ri.Cvar_Get("r_skyportal_origin", "0 0 0", 0);
	r_farplane = ri.Cvar_Get("r_farplane", "0", CVAR_CHEAT);
	r_farplane_bias = ri.Cvar_Get("r_farplane_bias", "0", CVAR_CHEAT);
	r_farplane_color = ri.Cvar_Get("r_farplane_color", ".5 .5 .5", CVAR_CHEAT);
	r_farplane_nocull = ri.Cvar_Get("r_farplane_nocull", "0", CVAR_CHEAT);
	r_farplane_nofog = ri.Cvar_Get("r_farplane_nofog", "0", CVAR_CHEAT);
	r_skybox_farplane = ri.Cvar_Get("r_skybox_farplane", "0", CVAR_CHEAT);
	r_farclip = ri.Cvar_Get("r_farclip", "0", CVAR_ARCHIVE);
	r_skyportal = ri.Cvar_Get("r_skyportal", "0", 0);
	r_skyportal_origin = ri.Cvar_Get("r_skyportal_origin", "0 0 0", 0);
	r_lightcoronasize = ri.Cvar_Get("r_lightcoronasize", ".1", CVAR_ARCHIVE);
	r_useglfog = ri.Cvar_Get("r_useglfog", "1", 64);
	r_debuglines_depthmask = ri.Cvar_Get("r_debuglines_depthmask", "0", CVAR_ARCHIVE);
	r_smoothsmokelight = ri.Cvar_Get("vss_smoothsmokelight", "1", CVAR_ARCHIVE);
	r_showportal = ri.Cvar_Get("r_showportal", "0", 0);
	sys_cpuid = ri.Cvar_Get("sys_cpuid", "0", 0);
	r_sse = ri.Cvar_Get("r_sse", "0", CVAR_ARCHIVE);
	r_static_shaderdata0 = ri.Cvar_Get("r_static_shaderdata0", "0", CVAR_SYSTEMINFO);
	r_static_shaderdata1 = ri.Cvar_Get("r_static_shaderdata1", "0", CVAR_SYSTEMINFO);
	r_static_shaderdata2 = ri.Cvar_Get("r_static_shaderdata2", "0", CVAR_SYSTEMINFO);
	r_static_shaderdata3 = ri.Cvar_Get("r_static_shaderdata3", "0", CVAR_SYSTEMINFO);
	r_static_shadermultiplier0 = ri.Cvar_Get("r_static_shadermultiplier0", "1", CVAR_SYSTEMINFO);
	r_static_shadermultiplier1 = ri.Cvar_Get("r_static_shadermultiplier1", "1", CVAR_SYSTEMINFO);
	r_static_shadermultiplier2 = ri.Cvar_Get("r_static_shadermultiplier2", "1", CVAR_SYSTEMINFO);
	r_static_shadermultiplier3 = ri.Cvar_Get("r_static_shadermultiplier3", "1", CVAR_SYSTEMINFO);
	r_precacheimages = ri.Cvar_Get("r_precacheimages", "0", 0);
	ter_minMarkRadius = ri.Cvar_Get("ter_minMarkRadius", "8", CVAR_ARCHIVE);
	ter_fastMarks = ri.Cvar_Get("ter_fastMarks", "1", CVAR_ARCHIVE);
	r_alpha_foliage1 = ri.Cvar_Get("r_alpha_foliage1", "0.75", CVAR_CHEAT);
	r_alpha_foliage2 = ri.Cvar_Get("r_alpha_foliage2", "0.75", CVAR_CHEAT);
	r_blendtrees = ri.Cvar_Get("r_blendtrees", "0", CVAR_LATCH);
	r_blendbushes = ri.Cvar_Get("r_blendbushes", "0", CVAR_LATCH);
	r_fps = ri.Cvar_Get("fps", "0", 0);
	r_loadjpg = ri.Cvar_Get("r_loadjpg", "1", CVAR_LATCH);
	r_loadftx = ri.Cvar_Get("r_loadftx", "0", CVAR_LATCH);

	r_ext_multisample = ri.Cvar_Get("r_ext_multisample", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_noborder = ri.Cvar_Get("r_noborder", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_ext_texture_filter_anisotropic = ri.Cvar_Get("r_ext_texture_filter_anisotropic",
		"0", CVAR_ARCHIVE | CVAR_LATCH);
	r_stereoEnabled = ri.Cvar_Get("r_stereoEnabled", "0", CVAR_ARCHIVE | CVAR_LATCH);

	// make sure all the commands added here are also
	// removed in R_Shutdown
	ri.Cmd_AddCommand( "imagelist", R_ImageList_f );
	ri.Cmd_AddCommand( "shaderlist", R_ShaderList_f );
	ri.Cmd_AddCommand( "skinlist", R_SkinList_f );
	ri.Cmd_AddCommand( "modellist", R_Modellist_f );
	ri.Cmd_AddCommand( "modelist", R_ModeList_f );
	ri.Cmd_AddCommand( "screenshot", R_ScreenShot_f );
	ri.Cmd_AddCommand( "screenshotJPEG", R_ScreenShotJPEG_f );
	ri.Cmd_AddCommand( "screenshotPNG", R_ScreenShotPNG_f );
	ri.Cmd_AddCommand( "gfxinfo", GfxInfo_f );
	ri.Cmd_AddCommand( "farplane_info", FarPlaneInfo_f );
	ri.Cmd_AddCommand( "r_infostaticmodels", R_InfoStaticModels_f );
	ri.Cmd_AddCommand( "r_infoworldtris", R_InfoWorldTris_f );

	//
	// Added in OPM
	//

	r_developer = ri.Cvar_Get("developer", "", 0);
    r_showSkeleton = ri.Cvar_Get("r_showSkeleton", "0", CVAR_CHEAT);
	r_aviMotionJpegQuality = ri.Cvar_Get("r_aviMotionJpegQuality", "90", CVAR_ARCHIVE);
    r_screenshotJpegQuality = ri.Cvar_Get("r_screenshotJpegQuality", "90", CVAR_ARCHIVE);

	ri.Cmd_AddCommand( "ter_crater", R_TerrainCrater_f );
}

void R_InitExtensions() {
}

/*
===============
R_Init
===============
*/
void R_Init( void ) {	
	int i;
	byte *ptr;

	ri.Printf( PRINT_ALL, "----- R_Init -----\n" );

	// clear all our internal state
	Com_Memset( &tr, 0, sizeof( tr ) );
	Com_Memset( &backEnd, 0, sizeof( backEnd ) );
	Com_Memset( &tess, 0, sizeof( tess ) );

//	Swap_Init();

	if ( (intptr_t)tess.xyz & 15 ) {
		ri.Printf(PRINT_ALL,  "WARNING: tess.xyz not 16 byte aligned\n" );
	}
	Com_Memset( tess.constantColor255, 255, sizeof( tess.constantColor255 ) );
	// the temporal path's history starts again with the backend's frames
	R_TemporalInit();

	tr.worldEntity.e.shaderRGBA[0] = -1;
	tr.worldEntity.e.shaderRGBA[1] = 0;
	tr.worldEntity.e.shaderRGBA[2] = -1;
	tr.worldEntity.e.shaderRGBA[3] = -1;

	//
	// init function tables
	//
	for ( i = 0; i < FUNCTABLE_SIZE; i++ )
	{
		tr.sinTable[i]		= sin( DEG2RAD( i * 360.0f / ( ( float ) ( FUNCTABLE_SIZE - 1 ) ) ) );
		tr.squareTable[i]	= ( i < FUNCTABLE_SIZE/2 ) ? 1.0f : -1.0f;
		tr.sawToothTable[i] = (float)i / FUNCTABLE_SIZE;
		tr.inverseSawToothTable[i] = 1.0f - tr.sawToothTable[i];

		if ( i < FUNCTABLE_SIZE / 2 )
		{
			if ( i < FUNCTABLE_SIZE / 4 )
			{
				tr.triangleTable[i] = ( float ) i / ( FUNCTABLE_SIZE / 4 );
			}
			else
			{
				tr.triangleTable[i] = 1.0f - tr.triangleTable[i-FUNCTABLE_SIZE / 4];
			}
		}
		else
		{
			tr.triangleTable[i] = -tr.triangleTable[i-FUNCTABLE_SIZE/2];
		}
	}

	tr.rendererhandle = ri.Milliseconds();

	if (!tr.rendererhandle) {
		tr.rendererhandle = -1;
	}

	R_NoiseInit();

	R_Sky_Init();

	R_Register();

	max_polys = r_maxpolys->integer;
	if (max_polys < MAX_POLYS)
		max_polys = MAX_POLYS;

	max_polyverts = r_maxpolyverts->integer;
	if (max_polyverts < MAX_POLYVERTS)
		max_polyverts = MAX_POLYVERTS;

	max_termarks = r_maxtermarks->integer;
	if (max_termarks < MAX_TERMARKS)
		max_termarks = MAX_TERMARKS;

	// two with r_smp: the render thread reads one while the front end
	// fills the other (R_InitNextFrame)
	for (i = 0; i < (r_smp->integer ? 2 : 1); i++) {
		size_t size = sizeof(*backEndData) + sizeof(srfPoly_t) * max_polys + sizeof(polyVert_t) * max_polyverts
			+ sizeof(srfMarkFragment_t) * max_termarks + sizeof(terrainSlotChange_t) * MAX_TERRAIN_CHANGES
			+ sizeof(glIndex_t) * MAX_TERRAIN_FRAME_INDEXES
			+ sizeof(skelBoneCache_t) * MAX_SKELBONES + sizeof(int) * MAX_SKELMORPH;
		backEndData_t *data;

		ptr = ri.Malloc(size);
		data = (backEndData_t*)ptr;
		data->polys = (srfPoly_t*)((char*)ptr + sizeof(*backEndData));
		data->polyVerts = (polyVert_t*)((char*)ptr + sizeof(*backEndData) + sizeof(srfPoly_t) * max_polys);
		data->terMarks = (srfMarkFragment_t*)((char*)ptr + sizeof(*backEndData) + sizeof(srfPoly_t) * max_polys + sizeof(polyVert_t) * max_polyverts);
		data->terrainChanges = (terrainSlotChange_t*)((char*)data->terMarks + sizeof(srfMarkFragment_t) * max_termarks);
		data->terrainIndexes = (glIndex_t*)((char*)data->terrainChanges + sizeof(terrainSlotChange_t) * MAX_TERRAIN_CHANGES);
		data->skelBones = (skelBoneCache_t*)((char*)data->terrainIndexes + sizeof(glIndex_t) * MAX_TERRAIN_FRAME_INDEXES);
		data->skelMorphs = (int*)((char*)data->skelBones + sizeof(skelBoneCache_t) * MAX_SKELBONES);
		data->staticModels = NULL;
		data->staticModelData = NULL;
		backEndDataArray[i] = data;
	}
	backEndDataArray[1] = backEndDataArray[1] ? backEndDataArray[1] : backEndDataArray[0];
	tr.smpFrame = 0;
	backEndData = backEndDataArray[0];
	R_InitNextFrame();

	InitGraphics();

	R_InitExtensions();

	R_InitImages();

	R_StartupShaders();

	R_ModelInit();

	R_LevelMarksInit();

	tr.pFontDebugStrings = R_LoadFont("verdana-14");
	g_bInfoworldtris = qfalse;

	// the render thread, once everything it uses exists
	R_StartRenderThread();

	ri.Printf( PRINT_ALL, "----- finished R_Init -----\n" );
}

/*
===============
RE_Shutdown
===============
*/
void RE_Shutdown( qboolean destroyWindow ) {	

	ri.Printf( PRINT_ALL, "RE_Shutdown( %i )\n", destroyWindow );

	ri.Cmd_RemoveCommand ("modellist");
	ri.Cmd_RemoveCommand ("screenshotJPEG");
	ri.Cmd_RemoveCommand ("screenshotPNG");
	ri.Cmd_RemoveCommand ("screenshot");
	ri.Cmd_RemoveCommand ("imagelist");
	ri.Cmd_RemoveCommand ("shaderlist");
	ri.Cmd_RemoveCommand ("skinlist");
	ri.Cmd_RemoveCommand ("gfxinfo");
	ri.Cmd_RemoveCommand( "modelist" );
	ri.Cmd_RemoveCommand( "shaderstate" );


	if ( tr.registered ) {
		R_IssuePendingRenderCommands();
		// the screenshots and video frames still in flight are written
		R_FlushReadbacks();
		R_DeleteTextures();
	}
	// the render thread has run its last list (R_IssuePendingRenderCommands)
	R_StopRenderThread();

	R_FreeStaticGeometry();
	R_ClearSkinData();

	// shut down the backend and the window
	if ( destroyWindow ) {
		vk_shutdown();
		VKimp_Shutdown();

		Com_Memset( &glConfig, 0, sizeof( glConfig ) );
		textureFilterAnisotropic = qfalse;
		maxAnisotropy = 0;
		displayAspect = 0.0f;
		haveClampToEdge = qfalse;

		Com_Memset( &glState, 0, sizeof( glState ) );
	}

	R_ShutdownFont();

	R_ShutdownTerrain();

	// what tess holds doesn't outlive the renderer
	R_Forget2DBatch();

	tr.registered = qfalse;
}

//extern qboolean scr_initialized;

/*
** RE_BeginRegistration
*/
void RE_BeginRegistration(glconfig_t* glconfigOut) {

	ri.UI_LoadResource("*123");
	//scr_initialized = 0;

	R_IssuePendingRenderCommands();

	R_LevelMarksFree();
	R_TerrainFree();

	ri.Hunk_Clear();

	*glconfigOut = glConfig;

	r_sequencenumber++;
	tr.viewCluster = -1;		// force markleafs to regenerate
	R_ClearFlares();
	RE_ClearScene();

	// Added in OPM
	//  Clear the world because some components
	//  like the mark system access the world object
	//  to free allocated marks
	R_ClearWorld();

	R_PrebuildReset();
	R_SetupShaders();
	R_InitLensFlare();
	R_LevelMarksInit();

	tr.registered = qtrue;

	//scr_initialized = 1;
	ri.UI_LoadResource("*124");
}

/*
=============
RE_EndRegistration

Touch all images to make sure they are resident
=============
*/
void RE_EndRegistration( void ) {
	R_IssuePendingRenderCommands();
	if (r_precacheimages->integer) {
		int start, end;

		start = ri.Milliseconds();

		RB_ShowImages(qtrue);

		end = ri.Milliseconds();

		ri.Printf(PRINT_ALL, "RB_ShowImages: %5.2f seconds\n", (float)((end - start) / 1000.f));
	}

	R_FreeUnusedImages();

	// what the registered models and shaders will draw: skin data and
	// pipelines now rather than at their first draw
	R_SkinRegisteredModels();
	R_PrebuildPipelines();
}

/*
=============
RE_SetRenderTime
=============
*/
void RE_SetRenderTime(int t) {
	if ( glConfig.smpActive ) {
		renderTimeCommand_t *cmd = R_GetCommandBuffer( sizeof( *cmd ) );

		if ( cmd ) {
			cmd->commandId = RC_SET_RENDER_TIME;
			cmd->t = t;
			cmd->refdefTime = tr.refdef.time;
		}
		return;
	}
	RB_SetRenderTime( t, tr.refdef.time );
}

// the render thread's RE_SetRenderTime (RC_SET_RENDER_TIME)
void RB_SetRenderTime( int t, int refdefTime ) {
	// the open 2D batch has the shader time it was made with
	R_End2DBatch();
	backEnd.refdef.floatTime = (long double)t / 1000.0;
	R_UpdateGhostTextures( refdefTime );
}

/*
@@@@@@@@@@@@@@@@@@@@@
GetRefAPI

@@@@@@@@@@@@@@@@@@@@@
*/
#ifdef USE_RENDERER_DLOPEN
Q_EXPORT refexport_t* QDECL GetRefAPI ( int apiVersion, refimport_t *rimp ) {
#else
refexport_t *GetRefAPI ( int apiVersion, refimport_t *rimp ) {
#endif

	static refexport_t	re;

	ri = *rimp;

	Com_Memset( &re, 0, sizeof( re ) );

	if ( apiVersion != REF_API_VERSION ) {
		ri.Printf(PRINT_ALL, "Mismatched REF_API_VERSION: expected %i, got %i\n", 
			REF_API_VERSION, apiVersion );
		return NULL;
	}

	R_Init();

	// the RE_ functions are Renderer Entry points

	re.Shutdown = RE_Shutdown;

	re.BeginRegistration = RE_BeginRegistration;
	re.RegisterModel = RE_RegisterModel;
	re.RegisterSkin = RE_RegisterSkin;
	re.RegisterShader = RE_RegisterShader;
	re.RegisterShaderNoMip = RE_RegisterShaderNoMip;
	re.LoadWorld = RE_LoadWorldMap;
	re.SetWorldVisData = RE_SetWorldVisData;
	re.EndRegistration = RE_EndRegistration;

	re.BeginFrame = RE_BeginFrame;
	re.EndFrame = RE_EndFrame;

	re.MarkFragments = R_MarkFragments;
	re.LerpTag = R_LerpTag;
	re.ModelBounds = R_ModelBounds;

	re.ClearScene = RE_ClearScene;
	re.AddRefEntityToScene = RE_AddRefEntityToScene;
	re.AddPolyToScene = RE_AddPolyToScene;
	re.LightForPoint = R_LightForPoint;
	re.AddLightToScene = RE_AddLightToScene;
	re.AddAdditiveLightToScene = RE_AddAdditiveLightToScene;
	re.RenderScene = RE_RenderScene;

	re.SetColor = RE_SetColor;
	re.DrawStretchPic = RE_StretchPic;
	re.DrawStretchRaw = RE_StretchRaw;
	re.UploadCinematic = RE_UploadCinematic;

	re.RegisterFont = RE_RegisterFont;
	re.RemapShader = R_RemapShader;
	re.GetEntityToken = R_GetEntityToken;
	re.inPVS = R_inPVS;

	re.TakeVideoFrame = RE_TakeVideoFrame;

	//
	// After ioquake3 imports
	//

    re.FreeModels = RE_FreeModels;
	re.EndRegistration = RE_EndRegistration;
    re.SpawnEffectModel = RE_SpawnEffectModel;
    re.RegisterServerModel = RE_RegisterServerModel;
    re.UnregisterServerModel = RE_UnregisterServerModel;
	re.RefreshShaderNoMip = RE_RefreshShaderNoMip;
	re.PrintBSPFileSizes = RE_PrintBSPFileSizes;
	re.MapVersion = RE_MapVersion;
    re.LoadFont = R_LoadFont;

    re.MarkFragmentsForInlineModel = R_MarkFragmentsForInlineModel;
    re.GetInlineModelBounds = R_GetInlineModelBounds;
    re.GetLightingForDecal = R_GetLightingForDecal;
    re.GetLightingForSmoke = R_GetLightingForSmoke;
    re.R_GatherLightSources = R_GatherLightSources;
    re.ModelRadius = R_ModelRadius;
    re.AddRefSpriteToScene = RE_AddRefSpriteToScene;
    re.AddTerrainMarkToScene = RE_AddTerrainMarkToScene;

    re.GetRenderEntity = RE_GetRenderEntity;

	re.SavePerformanceCounters = R_SavePerformanceCounters;

	re.R_Model_GetHandle = R_Model_GetHandle;
	re.SetColor = Draw_SetColor;
	re.DrawStretchPic = Draw_StretchPic;
	re.DrawStretchPic2 = Draw_StretchPic2;
	re.DrawStretchRaw = RE_StretchRaw;
	re.DebugLine = R_DebugLine;
	re.DrawTilePic = Draw_TilePic;
	re.DrawTilePicOffset = Draw_TilePicOffset;
	re.DrawTrianglePic = Draw_TrianglePic;
	re.DrawBox = DrawBox;
	re.AddBox = AddBox;
	re.DrawBoxes = DrawBoxes;
	re.Set2DWindow = Set2DWindow;
	re.Scissor = RE_Scissor;
	re.DrawLineLoop = DrawLineLoop;
	re.DrawString = R_DrawString;
	re.GetFontHeight = R_GetFontHeight;
	re.GetFontStringWidth = R_GetFontStringWidth;
	re.SwipeBegin = RE_SwipeBegin;
	re.SwipeEnd = RE_SwipeEnd;
	re.SetRenderTime = RE_SetRenderTime;
	re.Noise = R_NoiseGet4f;

	re.SetMode = R_SetMode;
	re.SetFullscreen = R_SetFullscreen;

	re.GetShaderHeight = RE_GetShaderHeight;
	re.GetShaderWidth = RE_GetShaderWidth;
	re.GetShaderName = RE_GetShaderName;
	re.GetModelName = RE_GetModelName;
	re.GetGraphicsInfo = RE_GetGraphicsInfo;
	re.ForceUpdatePose = RE_ForceUpdatePose;
	re.TIKI_Orientation = RE_TIKI_Orientation;
	re.TIKI_IsOnGround = RE_TIKI_IsOnGround;
	re.SetFrameNumber = RE_SetFrameNumber;

	re.ImageExists = R_ImageExists;
	re.CountTextureMemory = R_CountTextureMemory;

    re.LoadRawImage = R_LoadRawImage;
    re.FreeRawImage = R_FreeRawImage;

	re.Set2DInitialShaderTime = Set2DInitialShaderTime;

	return VK_FrameLogExports( &re );
}
