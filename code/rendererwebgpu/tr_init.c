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
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
/*
 * WebGPU renderer stub for Emscripten.
 *
 * This file provides the renderer interface (refexport_t) backed by WebGPU
 * instead of OpenGL.  It is compiled when USE_WEBGPU=1 is set at build time
 * and replaces the renderergl2 + sdl_glimp objects in the link.
 *
 * Current state: scaffolding / stub.  Every refexport_t entry-point is
 * present so the binary links and launches; actual WebGPU draw calls are
 * left as TODO placeholders.
 */

#ifdef USE_WEBGPU

#include "../qcommon/q_shared.h"
#include "../qcommon/qfiles.h"
#include "../renderercommon/tr_public.h"

#ifdef __EMSCRIPTEN__
#include <SDL.h>
#include <emscripten/html5_webgpu.h>
#include <webgpu/webgpu.h>
#endif

/* =========================================================================
 * Module-level state
 * ========================================================================= */

static refimport_t ri;
static glconfig_t  glConfig;

#ifdef __EMSCRIPTEN__
static SDL_Window *webgpu_window  = NULL;
static WGPUDevice  webgpu_device  = NULL;
static WGPUSurface webgpu_surface = NULL;
static WGPUQueue   webgpu_queue   = NULL;
#endif

/* =========================================================================
 * GLimp stubs
 *
 * The client expects these symbols whether or not they make sense for a
 * WebGPU renderer.  We provide minimal implementations so that the link
 * succeeds and window/input management still works through SDL.
 * ========================================================================= */

void GLimp_LogComment( char *comment )
{
	(void)comment;
}

void GLimp_Minimize( void )
{
#ifdef __EMSCRIPTEN__
	if ( webgpu_window )
		SDL_MinimizeWindow( webgpu_window );
#endif
}

void GLimp_Shutdown( void )
{
	ri.IN_Shutdown();

#ifdef __EMSCRIPTEN__
	if ( webgpu_surface )
	{
		wgpuSurfaceRelease( webgpu_surface );
		webgpu_surface = NULL;
	}
	if ( webgpu_device )
	{
		wgpuDeviceRelease( webgpu_device );
		webgpu_device = NULL;
	}
	if ( webgpu_window )
	{
		SDL_DestroyWindow( webgpu_window );
		webgpu_window = NULL;
	}
	SDL_QuitSubSystem( SDL_INIT_VIDEO );
#endif
}

/*
 * GLimp_Init -- create the SDL window and acquire a WebGPU device/surface.
 *
 * fixedFunction is ignored; WebGPU always uses programmable shaders.
 */
void GLimp_Init( qboolean fixedFunction )
{
	(void)fixedFunction;

#ifdef __EMSCRIPTEN__
	cvar_t *r_mode         = ri.Cvar_Get( "r_mode",         "-2", CVAR_ARCHIVE | CVAR_LATCH );
	cvar_t *r_fullscreen   = ri.Cvar_Get( "r_fullscreen",   "0",  CVAR_ARCHIVE | CVAR_LATCH );
	cvar_t *r_width        = ri.Cvar_Get( "r_width",        "1280", CVAR_ARCHIVE | CVAR_LATCH );
	cvar_t *r_height       = ri.Cvar_Get( "r_height",       "720",  CVAR_ARCHIVE | CVAR_LATCH );

	int width  = r_width->integer  > 0 ? r_width->integer  : 1280;
	int height = r_height->integer > 0 ? r_height->integer : 720;
	(void)r_mode;
	(void)r_fullscreen;

	if ( SDL_Init( SDL_INIT_VIDEO ) < 0 )
	{
		ri.Error( ERR_FATAL, "GLimp_Init: SDL_Init failed: %s", SDL_GetError() );
		return;
	}

	/* Create a plain SDL window (no OpenGL flag – WebGPU manages the surface). */
	webgpu_window = SDL_CreateWindow(
		CLIENT_WINDOW_TITLE,
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		width, height,
		SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
	);
	if ( !webgpu_window )
	{
		ri.Error( ERR_FATAL, "GLimp_Init: SDL_CreateWindow failed: %s", SDL_GetError() );
		return;
	}

	/* Acquire a WebGPU device through the Emscripten helper. */
	webgpu_device = emscripten_webgpu_get_device();
	if ( !webgpu_device )
	{
		ri.Error( ERR_FATAL, "GLimp_Init: emscripten_webgpu_get_device() returned NULL" );
		return;
	}

	webgpu_queue = wgpuDeviceGetQueue( webgpu_device );

	/* Create a surface that targets the HTML canvas element. */
	WGPUSurfaceDescriptorFromCanvasHTMLSelector canvasDesc = {
		.chain    = { .sType = WGPUSType_SurfaceDescriptorFromCanvasHTMLSelector },
		.selector = "#canvas",
	};
	WGPUSurfaceDescriptor surfaceDesc = {
		.nextInChain = &canvasDesc.chain,
	};
	WGPUInstance instance = wgpuCreateInstance( NULL );
	webgpu_surface = wgpuInstanceCreateSurface( instance, &surfaceDesc );
	wgpuInstanceRelease( instance );

	if ( !webgpu_surface )
	{
		ri.Error( ERR_FATAL, "GLimp_Init: wgpuInstanceCreateSurface() failed" );
		return;
	}

	/* Populate glConfig with sane defaults so the rest of the engine
	   can read resolution / capability information. */
	Com_Memset( &glConfig, 0, sizeof( glConfig ) );
	Q_strncpyz( glConfig.renderer_string,  "WebGPU",   sizeof( glConfig.renderer_string ) );
	Q_strncpyz( glConfig.vendor_string,    "Browser",  sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.version_string,   "1.0",      sizeof( glConfig.version_string ) );
	glConfig.vidWidth            = width;
	glConfig.vidHeight           = height;
	glConfig.windowAspect        = (float)width / (float)height;
	glConfig.colorBits           = 32;
	glConfig.depthBits           = 24;
	glConfig.stencilBits         = 8;
	glConfig.maxTextureSize      = 8192;
	glConfig.numTextureUnits     = 16;
	glConfig.driverType          = GLDRV_ICD;
	glConfig.hardwareType        = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = qfalse;
	glConfig.textureCompression  = TC_NONE;

	/* Hand the SDL window to the input subsystem. */
	ri.IN_Init( webgpu_window );

	ri.Printf( PRINT_ALL, "WebGPU renderer initialised (%dx%d)\n", width, height );
#else
	ri.Error( ERR_FATAL, "WebGPU renderer is only supported on Emscripten" );
#endif
}

/* Present the current frame.  With WebGPU this happens via wgpuQueueSubmit /
   wgpuSurfacePresent; for now it is a no-op stub until draw commands are
   generated. */
void GLimp_EndFrame( void )
{
	/* TODO: wgpuSurfacePresent( webgpu_surface ); */
}

/* Called after GLimp_Init to load any extra extensions.  WebGPU has no
   extension string mechanism; nothing to do here. */
void GLimp_InitExtraExtensions( void )
{
}

/* =========================================================================
 * Renderer interface (refexport_t) stubs
 *
 * Every function pointer in refexport_t must be non-NULL; we provide minimal
 * implementations here.  Functions that the engine needs actual output from
 * (BeginRegistration, LerpTag, …) return safe zero / default values.
 * ========================================================================= */

static void RE_Shutdown( qboolean destroyWindow )
{
	ri.Printf( PRINT_ALL, "RE_Shutdown( %i )\n", destroyWindow );
	if ( destroyWindow )
		GLimp_Shutdown();
}

static void RE_BeginRegistration( glconfig_t *config )
{
	GLimp_Init( qfalse );
	*config = glConfig;
}

static void RE_GetGlConfig( glconfig_t *config )
{
	*config = glConfig;
}

static qhandle_t RE_RegisterModel( const char *name )         { (void)name; return 0; }
static void      R_GetModelName( qhandle_t index, char *name, int sz )
                                                               { (void)index; if(sz>0) name[0]='\0'; }
static qhandle_t RE_RegisterSkin( const char *name )          { (void)name; return 0; }
static qhandle_t RE_RegisterShader( const char *name )        { (void)name; return 0; }
static qhandle_t RE_RegisterShaderNoMip( const char *name )   { (void)name; return 0; }
static qhandle_t RE_RegisterShaderLightMap( const char *name, int lightmap )
                                                               { (void)name; (void)lightmap; return 0; }
static void      RE_LoadWorldMap( const char *name )           { (void)name; }
static void      RE_SetWorldVisData( const byte *vis )         { (void)vis; }
static void      RE_EndRegistration( void )                    {}

static void      RE_BeginFrame( stereoFrame_t stereoFrame, qboolean recordingVideo )
                                                               { (void)stereoFrame; (void)recordingVideo; }
static void      RE_EndFrame( int *frontEndMsec, int *backEndMsec )
{
	if ( frontEndMsec ) *frontEndMsec = 0;
	if ( backEndMsec )  *backEndMsec  = 0;
	GLimp_EndFrame();
}

static int  R_MarkFragments( int numPoints, const vec3_t *points, const vec3_t projection,
                              int maxPoints, vec3_t pointBuffer, int maxFragments,
                              markFragment_t *fragmentBuffer )
{
	(void)numPoints; (void)points; (void)projection;
	(void)maxPoints; (void)pointBuffer; (void)maxFragments; (void)fragmentBuffer;
	return 0;
}

static int  R_LerpTag( orientation_t *tag, qhandle_t model, int startFrame, int endFrame,
                        float frac, const char *tagName )
{
	(void)model; (void)startFrame; (void)endFrame; (void)frac; (void)tagName;
	Com_Memset( tag, 0, sizeof( *tag ) );
	return 0;
}

static void R_ModelBounds( qhandle_t model, vec3_t mins, vec3_t maxs )
{
	(void)model;
	VectorClear( mins );
	VectorClear( maxs );
}

static void RE_ClearScene( void )                              {}
static void RE_AddRefEntityToScene( const refEntity_t *re )   { (void)re; }
static void RE_AddRefEntityPtrToScene( refEntity_t *re )       { (void)re; }
static void RE_SetPathLines( int *numCameraPoints, cameraPoint_t *cameraPoints,
                              int *numSplinePoints, vec3_t *splinePoints,
                              const vec4_t color )
{
	(void)numCameraPoints; (void)cameraPoints;
	(void)numSplinePoints; (void)splinePoints; (void)color;
}
static void RE_AddPolyToScene( qhandle_t hShader, int numVerts, const polyVert_t *verts,
                                int num, int lightmap )
{
	(void)hShader; (void)numVerts; (void)verts; (void)num; (void)lightmap;
}
static int  R_LightForPoint( const vec3_t point, vec3_t ambientLight,
                               vec3_t directedLight, vec3_t lightDir )
{
	(void)point;
	VectorClear( ambientLight );
	VectorClear( directedLight );
	VectorClear( lightDir );
	return 0;
}
static void RE_AddLightToScene( const vec3_t org, float intensity, float r, float g, float b )
                                                               { (void)org; (void)intensity; (void)r; (void)g; (void)b; }
static void RE_AddAdditiveLightToScene( const vec3_t org, float intensity, float r, float g, float b )
                                                               { (void)org; (void)intensity; (void)r; (void)g; (void)b; }
static void RE_RenderScene( const refdef_t *fd )               { (void)fd; }

static void RE_SetColor( const float *rgba )                   { (void)rgba; }
static void RE_StretchPic( float x, float y, float w, float h,
                            float s1, float t1, float s2, float t2,
                            qhandle_t hShader )
{
	(void)x; (void)y; (void)w; (void)h;
	(void)s1; (void)t1; (void)s2; (void)t2; (void)hShader;
}
static void RE_StretchRaw( int x, int y, int w, int h, int cols, int rows,
                            const byte *data, int client, qboolean dirty )
{
	(void)x; (void)y; (void)w; (void)h; (void)cols; (void)rows;
	(void)data; (void)client; (void)dirty;
}
static void RE_UploadCinematic( int w, int h, int cols, int rows,
                                 const byte *data, int client, qboolean dirty )
{
	(void)w; (void)h; (void)cols; (void)rows; (void)data; (void)client; (void)dirty;
}

static void     RE_RegisterFont( const char *fontName, int pointSize, fontInfo_t *font )
                                                               { (void)fontName; (void)pointSize; Com_Memset( font, 0, sizeof(*font) ); }
static qboolean RE_GetGlyphInfo( fontInfo_t *fontInfo, int charValue, glyphInfo_t *glyphOut )
                                                               { (void)fontInfo; (void)charValue; Com_Memset( glyphOut, 0, sizeof(*glyphOut) ); return qfalse; }
static qboolean RE_GetFontInfo( int fontId, fontInfo_t *font )
                                                               { (void)fontId; Com_Memset( font, 0, sizeof(*font) ); return qfalse; }

static void     R_RemapShader( const char *oldShader, const char *newShader,
                                const char *offsetTime, qboolean keepLightmap,
                                qboolean userSet )
{
	(void)oldShader; (void)newShader; (void)offsetTime;
	(void)keepLightmap; (void)userSet;
}
static void     R_ClearRemappedShader( const char *shaderName )
                                                               { (void)shaderName; }
static qboolean R_GetEntityToken( char *buffer, int size )
                                                               { if(size>0) buffer[0]='\0'; return qfalse; }
static qboolean R_inPVS( const vec3_t p1, const vec3_t p2 )   { (void)p1; (void)p2; return qtrue; }

static void     RE_TakeVideoFrame( aviFileData_t *afd, int h, int w,
                                    byte *captureBuffer, byte *encodeBuffer,
                                    qboolean motionJpeg, qboolean avi,
                                    qboolean tga, qboolean jpg, qboolean png,
                                    int picCount, char *givenFileName )
{
	(void)afd; (void)h; (void)w; (void)captureBuffer; (void)encodeBuffer;
	(void)motionJpeg; (void)avi; (void)tga; (void)jpg; (void)png;
	(void)picCount; (void)givenFileName;
}

static void     RE_BeginHud( void )                            {}
static void     RE_UpdateDof( float viewFocus, float viewRadius )
                                                               { (void)viewFocus; (void)viewRadius; }

static void     RE_Get_Advertisements( int *num, float *verts, char shaders[][MAX_QPATH] )
                                                               { (void)verts; (void)shaders; if(num) *num = 0; }
static void     RE_ReplaceShaderImage( qhandle_t h, const ubyte *data, int width, int height )
                                                               { (void)h; (void)data; (void)width; (void)height; }
static qhandle_t RE_RegisterShaderFromData( const char *name, ubyte *data, int width, int height,
                                             qboolean mipmap, qboolean allowPicmip,
                                             int wrapClampMode, int lightmapIndex )
{
	(void)name; (void)data; (void)width; (void)height;
	(void)mipmap; (void)allowPicmip; (void)wrapClampMode; (void)lightmapIndex;
	return 0;
}
static void     RE_GetShaderImageDimensions( qhandle_t h, int *width, int *height )
                                                               { (void)h; if(width)*width=0; if(height)*height=0; }
static void     RE_GetShaderImageData( qhandle_t h, ubyte *data )
                                                               { (void)h; (void)data; }
static qhandle_t RE_GetSingleShader( void )                    { return 0; }

/* =========================================================================
 * GetRefAPI -- the single exported symbol
 * ========================================================================= */

#ifdef USE_RENDERER_DLOPEN
Q_EXPORT refexport_t * QDECL GetRefAPI( int apiVersion, refimport_t *rimp )
{
#else
refexport_t *GetRefAPI( int apiVersion, refimport_t *rimp )
{
#endif
	static refexport_t re;

	ri = *rimp;

	Com_Memset( &re, 0, sizeof( re ) );

	if ( apiVersion != REF_API_VERSION )
	{
		ri.Printf( PRINT_ALL,
		           "Mismatched REF_API_VERSION: expected %i, got %i\n",
		           REF_API_VERSION, apiVersion );
		return NULL;
	}

	re.Shutdown               = RE_Shutdown;
	re.BeginRegistration      = RE_BeginRegistration;
	re.GetGlConfig            = RE_GetGlConfig;
	re.RegisterModel          = RE_RegisterModel;
	re.GetModelName           = R_GetModelName;
	re.RegisterSkin           = RE_RegisterSkin;
	re.RegisterShader         = RE_RegisterShader;
	re.RegisterShaderLightMap = RE_RegisterShaderLightMap;
	re.RegisterShaderNoMip    = RE_RegisterShaderNoMip;
	re.LoadWorld              = RE_LoadWorldMap;
	re.SetWorldVisData        = RE_SetWorldVisData;
	re.EndRegistration        = RE_EndRegistration;

	re.BeginFrame             = RE_BeginFrame;
	re.EndFrame               = RE_EndFrame;

	re.MarkFragments          = R_MarkFragments;
	re.LerpTag                = R_LerpTag;
	re.ModelBounds            = R_ModelBounds;

	re.ClearScene             = RE_ClearScene;
	re.AddRefEntityToScene    = RE_AddRefEntityToScene;
	re.AddRefEntityPtrToScene = RE_AddRefEntityPtrToScene;
	re.SetPathLines           = RE_SetPathLines;
	re.AddPolyToScene         = RE_AddPolyToScene;
	re.LightForPoint          = R_LightForPoint;
	re.AddLightToScene        = RE_AddLightToScene;
	re.AddAdditiveLightToScene= RE_AddAdditiveLightToScene;
	re.RenderScene            = RE_RenderScene;

	re.SetColor               = RE_SetColor;
	re.DrawStretchPic         = RE_StretchPic;
	re.DrawStretchRaw         = RE_StretchRaw;
	re.UploadCinematic        = RE_UploadCinematic;

	re.RegisterFont           = RE_RegisterFont;
	re.GetGlyphInfo           = RE_GetGlyphInfo;
	re.GetFontInfo            = RE_GetFontInfo;
	re.RemapShader            = R_RemapShader;
	re.ClearRemappedShader    = R_ClearRemappedShader;
	re.GetEntityToken         = R_GetEntityToken;
	re.inPVS                  = R_inPVS;

	re.TakeVideoFrame         = RE_TakeVideoFrame;

	re.BeginHud               = RE_BeginHud;
	re.UpdateDof              = RE_UpdateDof;

	re.Get_Advertisements     = RE_Get_Advertisements;
	re.ReplaceShaderImage     = RE_ReplaceShaderImage;
	re.RegisterShaderFromData = RE_RegisterShaderFromData;
	re.GetShaderImageDimensions = RE_GetShaderImageDimensions;
	re.GetShaderImageData     = RE_GetShaderImageData;
	re.GetSingleShader        = RE_GetSingleShader;

	return &re;
}

#endif /* USE_WEBGPU */
