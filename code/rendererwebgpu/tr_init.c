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
 * tr_init.c – WebGPU renderer initialisation and refexport_t interface.
 *
 * Compiled only when USE_WEBGPU=1.  Links the WebGPU device / swap-chain
 * setup with the Q3 renderer interface so the engine can use WebGPU instead
 * of OpenGL/WebGL.
 */

#ifdef USE_WEBGPU

#include "tr_local.h"

/* =========================================================================
 * Global state (definitions – declared extern in tr_local.h)
 * ========================================================================= */

wgpuGlobals_t  wgpu;
refimport_t    ri;
glconfig_t     glConfig;

/* Required by renderercommon (tr_subs.c, tr_noise.c, tr_font.c, …) */
qboolean textureFilterAnisotropic = qfalse;
int      maxAnisotropy            = 0;
float    displayAspect            = 1.0f;
qboolean haveClampToEdge          = qtrue;

/* =========================================================================
 * GLimp – platform / window interface
 * ========================================================================= */

void GLimp_LogComment( char *comment )
{
    (void)comment;
}

void GLimp_Minimize( void )
{
#ifdef __EMSCRIPTEN__
    if ( wgpu.window )
        SDL_MinimizeWindow( wgpu.window );
#endif
}

void GLimp_InitExtraExtensions( void )
{
    /* WebGPU has no extension string mechanism */
}

/* -------------------------------------------------------------------------
 * GLimp_Shutdown – release WebGPU resources and destroy the SDL window.
 * ---------------------------------------------------------------------- */
void GLimp_Shutdown( void )
{
    ri.IN_Shutdown();

#ifdef __EMSCRIPTEN__
    WR_FreeImages();

    WR_ClearWorld();
    WR_FreeModels();

    if ( wgpu.verts2D    ) { ri.Free( wgpu.verts2D );    wgpu.verts2D    = NULL; }
    if ( wgpu.dynVerts3D ) { ri.Free( wgpu.dynVerts3D ); wgpu.dynVerts3D = NULL; }
    if ( wgpu.dynIndexes3D ) { ri.Free( wgpu.dynIndexes3D ); wgpu.dynIndexes3D = NULL; }
    if ( wgpu.ub3DData   ) { ri.Free( wgpu.ub3DData );   wgpu.ub3DData   = NULL; }

    if ( wgpu.vb2D )
    {
        wgpuBufferDestroy( wgpu.vb2D );
        wgpuBufferRelease( wgpu.vb2D );
        wgpu.vb2D = NULL;
    }
    if ( wgpu.ub2D )
    {
        wgpuBufferDestroy( wgpu.ub2D );
        wgpuBufferRelease( wgpu.ub2D );
        wgpu.ub2D = NULL;
    }
    if ( wgpu.dynVB3D )
    {
        wgpuBufferDestroy( wgpu.dynVB3D );
        wgpuBufferRelease( wgpu.dynVB3D );
        wgpu.dynVB3D = NULL;
    }
    if ( wgpu.dynIB3D )
    {
        wgpuBufferDestroy( wgpu.dynIB3D );
        wgpuBufferRelease( wgpu.dynIB3D );
        wgpu.dynIB3D = NULL;
    }
    if ( wgpu.ub3D )
    {
        wgpuBufferDestroy( wgpu.ub3D );
        wgpuBufferRelease( wgpu.ub3D );
        wgpu.ub3D = NULL;
    }

    if ( wgpu.depthView )
    {
        wgpuTextureViewRelease( wgpu.depthView );
        wgpu.depthView = NULL;
    }
    if ( wgpu.depthTex )
    {
        wgpuTextureDestroy( wgpu.depthTex );
        wgpuTextureRelease( wgpu.depthTex );
        wgpu.depthTex = NULL;
    }

    if ( wgpu.shaderMod2D )
    {
        wgpuShaderModuleRelease( wgpu.shaderMod2D );
        wgpu.shaderMod2D = NULL;
    }
    if ( wgpu.shaderMod3D )
    {
        wgpuShaderModuleRelease( wgpu.shaderMod3D );
        wgpu.shaderMod3D = NULL;
    }

    if ( wgpu.swapChain )
    {
        wgpuSwapChainRelease( wgpu.swapChain );
        wgpu.swapChain = NULL;
    }
    if ( wgpu.surface )
    {
        wgpuSurfaceRelease( wgpu.surface );
        wgpu.surface = NULL;
    }
    if ( wgpu.queue )
    {
        wgpuQueueRelease( wgpu.queue );
        wgpu.queue = NULL;
    }
    if ( wgpu.device )
    {
        wgpuDeviceRelease( wgpu.device );
        wgpu.device = NULL;
    }
    if ( wgpu.window )
    {
        SDL_DestroyWindow( wgpu.window );
        wgpu.window = NULL;
        SDL_QuitSubSystem( SDL_INIT_VIDEO );
    }
#endif

    wgpu.initialized = qfalse;
}

/* -------------------------------------------------------------------------
 * GLimp_EndFrame – present the rendered frame.
 * (Called from RE_EndFrame → WR_EndFrame, so this is a no-op here.)
 * ---------------------------------------------------------------------- */
void GLimp_EndFrame( void )
{
    /* Presentation is handled by WR_EndFrame. */
}

/* -------------------------------------------------------------------------
 * GLimp_Init – create SDL window, acquire WebGPU device/surface/swapchain,
 * upload all GPU resources needed for the 2D pass.
 * ---------------------------------------------------------------------- */
void GLimp_Init( qboolean fixedFunction )
{
    (void)fixedFunction;

#ifdef __EMSCRIPTEN__
    cvar_t *r_width  = ri.Cvar_Get( "r_width",  "1280", CVAR_ARCHIVE | CVAR_LATCH );
    cvar_t *r_height = ri.Cvar_Get( "r_height", "720",  CVAR_ARCHIVE | CVAR_LATCH );

    int width  = ( r_width->integer  > 0 ) ? r_width->integer  : 1280;
    int height = ( r_height->integer > 0 ) ? r_height->integer : 720;

    /* ------------------------------------------------------------------ */
    if ( SDL_Init( SDL_INIT_VIDEO ) < 0 )
        ri.Error( ERR_FATAL, "GLimp_Init: SDL_Init failed: %s", SDL_GetError() );

    wgpu.window = SDL_CreateWindow(
        CLIENT_WINDOW_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if ( !wgpu.window )
        ri.Error( ERR_FATAL, "GLimp_Init: SDL_CreateWindow failed: %s", SDL_GetError() );

    wgpu.vidWidth  = width;
    wgpu.vidHeight = height;

    /* ---- Acquire WebGPU device ---------------------------------------- */
    wgpu.device = emscripten_webgpu_get_device();
    if ( !wgpu.device )
        ri.Error( ERR_FATAL, "GLimp_Init: emscripten_webgpu_get_device() returned NULL" );

    wgpu.queue = wgpuDeviceGetQueue( wgpu.device );

    /* ---- Create surface targeting the HTML canvas ---------------------- */
    {
        WGPUSurfaceDescriptorFromCanvasHTMLSelector canvasDesc;
        WGPUSurfaceDescriptor                       surfDesc;
        WGPUInstance                                instance;

        Com_Memset( &canvasDesc, 0, sizeof( canvasDesc ) );
        canvasDesc.chain.sType = WGPUSType_SurfaceDescriptorFromCanvasHTMLSelector;
        canvasDesc.selector    = "#canvas";

        Com_Memset( &surfDesc, 0, sizeof( surfDesc ) );
        surfDesc.nextInChain = &canvasDesc.chain;

        instance      = wgpuCreateInstance( NULL );
        wgpu.surface  = wgpuInstanceCreateSurface( instance, &surfDesc );
        wgpuInstanceRelease( instance );

        if ( !wgpu.surface )
            ri.Error( ERR_FATAL, "GLimp_Init: wgpuInstanceCreateSurface() failed" );
    }

    /* ---- Create swap chain --------------------------------------------- */
    {
        WGPUSwapChainDescriptor scDesc;
        Com_Memset( &scDesc, 0, sizeof( scDesc ) );
        scDesc.usage       = WGPUTextureUsage_RenderAttachment;
        scDesc.format      = WGPUTextureFormat_BGRA8Unorm;
        scDesc.width       = (uint32_t)width;
        scDesc.height      = (uint32_t)height;
        scDesc.presentMode = WGPUPresentMode_Fifo;
        scDesc.label       = "Main swap chain";

        wgpu.swapChain = wgpuDeviceCreateSwapChain( wgpu.device, wgpu.surface, &scDesc );
        if ( !wgpu.swapChain )
            ri.Error( ERR_FATAL, "GLimp_Init: wgpuDeviceCreateSwapChain() failed" );
    }

    /* ---- Depth texture ------------------------------------------------- */
    {
        WGPUTextureDescriptor     tdesc;
        WGPUTextureViewDescriptor vdesc;

        Com_Memset( &tdesc, 0, sizeof( tdesc ) );
        tdesc.usage               = WGPUTextureUsage_RenderAttachment;
        tdesc.dimension           = WGPUTextureDimension_2D;
        tdesc.size.width          = (uint32_t)width;
        tdesc.size.height         = (uint32_t)height;
        tdesc.size.depthOrArrayLayers = 1;
        tdesc.format              = WGPUTextureFormat_Depth24PlusStencil8;
        tdesc.mipLevelCount       = 1;
        tdesc.sampleCount         = 1;
        tdesc.label               = "Depth/stencil";

        wgpu.depthTex = wgpuDeviceCreateTexture( wgpu.device, &tdesc );

        Com_Memset( &vdesc, 0, sizeof( vdesc ) );
        vdesc.format          = WGPUTextureFormat_Depth24PlusStencil8;
        vdesc.dimension       = WGPUTextureViewDimension_2D;
        vdesc.baseMipLevel    = 0;
        vdesc.mipLevelCount   = 1;
        vdesc.baseArrayLayer  = 0;
        vdesc.arrayLayerCount = 1;
        vdesc.aspect          = WGPUTextureAspect_All;
        vdesc.label           = "Depth view";

        wgpu.depthView = wgpuTextureCreateView( wgpu.depthTex, &vdesc );
    }

    /* ---- 2D vertex buffer (write-mapped each frame) ------------------- */
    {
        WGPUBufferDescriptor bdesc;
        Com_Memset( &bdesc, 0, sizeof( bdesc ) );
        bdesc.usage            = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        bdesc.size             = WGPU_2D_VB_SIZE;
        bdesc.mappedAtCreation = 0;
        bdesc.label            = "2D vertex buffer";
        wgpu.vb2D = wgpuDeviceCreateBuffer( wgpu.device, &bdesc );
    }

    /* ---- 2D uniform buffer -------------------------------------------- */
    {
        WGPUBufferDescriptor bdesc;
        Com_Memset( &bdesc, 0, sizeof( bdesc ) );
        bdesc.usage            = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bdesc.size             = WGPU_UNIFORM_BUF_SIZE;
        bdesc.mappedAtCreation = 0;
        bdesc.label            = "2D uniform buffer";
        wgpu.ub2D = wgpuDeviceCreateBuffer( wgpu.device, &bdesc );
    }

    /* ---- CPU-side staging for 2D vertices ----------------------------- */
    wgpu.verts2D = (wgpuVert2D_t *)ri.Malloc( MAX_2D_VERTS * sizeof( wgpuVert2D_t ) );
    if ( !wgpu.verts2D )
        ri.Error( ERR_FATAL, "GLimp_Init: failed to allocate 2D vertex staging buffer" );

    /* ---- 3D per-frame dynamic vertex buffer --------------------------- */
    {
        WGPUBufferDescriptor bdesc;
        Com_Memset( &bdesc, 0, sizeof( bdesc ) );
        bdesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        bdesc.size  = (uint64_t)WGPU_MAX_DYN_VERTS3D * sizeof( wgpuVert3D_t );
        bdesc.label = "3D dynamic vertex buffer";
        wgpu.dynVB3D = wgpuDeviceCreateBuffer( wgpu.device, &bdesc );
    }

    /* ---- 3D per-frame dynamic index buffer ---------------------------- */
    {
        WGPUBufferDescriptor bdesc;
        Com_Memset( &bdesc, 0, sizeof( bdesc ) );
        bdesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
        bdesc.size  = (uint64_t)WGPU_MAX_DYN_INDEXES3D * sizeof( int );
        bdesc.label = "3D dynamic index buffer";
        wgpu.dynIB3D = wgpuDeviceCreateBuffer( wgpu.device, &bdesc );
    }

    /* ---- 3D uniform buffer ------------------------------------------- */
    {
        WGPUBufferDescriptor bdesc;
        Com_Memset( &bdesc, 0, sizeof( bdesc ) );
        bdesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bdesc.size  = (uint64_t)WGPU_MAX_DRAWS3D * WGPU_3D_UB_STRIDE;
        bdesc.label = "3D uniform buffer";
        wgpu.ub3D = wgpuDeviceCreateBuffer( wgpu.device, &bdesc );
    }

    /* ---- CPU-side staging for 3D ---------------------------------------- */
    wgpu.dynVerts3D = (wgpuVert3D_t *)ri.Malloc(
        WGPU_MAX_DYN_VERTS3D * sizeof( wgpuVert3D_t ) );
    wgpu.dynIndexes3D = (int *)ri.Malloc(
        WGPU_MAX_DYN_INDEXES3D * sizeof( int ) );
    wgpu.ub3DData = (byte *)ri.Malloc(
        WGPU_MAX_DRAWS3D * WGPU_3D_UB_STRIDE );

    if ( !wgpu.dynVerts3D || !wgpu.dynIndexes3D || !wgpu.ub3DData )
        ri.Error( ERR_FATAL, "GLimp_Init: failed to allocate 3D staging buffers" );

    /* ---- WGSL shader modules + pipelines ------------------------------ */
    WR_InitShaderModules();
    WR_InitPipelines();

    /* ---- Image + material system --------------------------------------- */
    WR_InitImages();
    WR_InitShaders();

    /* ---- Populate glConfig -------------------------------------------- */
    Com_Memset( &glConfig, 0, sizeof( glConfig ) );
    Q_strncpyz( glConfig.renderer_string, "WebGPU",   sizeof( glConfig.renderer_string ) );
    Q_strncpyz( glConfig.vendor_string,   "Browser",  sizeof( glConfig.vendor_string   ) );
    Q_strncpyz( glConfig.version_string,  "1.0",      sizeof( glConfig.version_string  ) );
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

    /* ---- Default 2D colour -------------------------------------------- */
    wgpu.color2D[0] = wgpu.color2D[1] = wgpu.color2D[2] = wgpu.color2D[3] = 1.0f;

    /* ---- Input system ------------------------------------------------- */
    ri.IN_Init( wgpu.window );

    wgpu.initialized = qtrue;
    ri.Printf( PRINT_ALL, "WebGPU renderer initialised (%dx%d)\n", width, height );

#else
    ri.Error( ERR_FATAL, "WebGPU renderer requires Emscripten" );
#endif
}

/* =========================================================================
 * refexport_t implementations
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

static qhandle_t RE_RegisterModel( const char *name )
{
    int idx;
    if ( !wgpu.initialized || !name || !name[0] ) return 0;
    idx = WR_LoadModel( name );
    return ( idx >= 0 ) ? (qhandle_t)( idx + 1 ) : 0;  /* +1 so handle 0 = invalid */
}

static void R_GetModelName( qhandle_t index, char *name, int sz )
{
    int modelIdx = (int)index - 1;  /* handle 0 = invalid; model 0 has handle 1 */
    if ( modelIdx >= 0 && modelIdx < wgpu.numModels && wgpu.models[modelIdx].loaded )
        Q_strncpyz( name, wgpu.models[modelIdx].name, sz );
    else if ( sz > 0 )
        name[0] = '\0';
}

static qhandle_t RE_RegisterSkin( const char *name )
{
    (void)name;
    return 0;
}

static qhandle_t RE_RegisterShader( const char *name )
{
    if ( !wgpu.initialized ) return 0;
    return (qhandle_t)WR_RegisterShader( name, WGPU_MAT_BLEND );
}

static qhandle_t RE_RegisterShaderNoMip( const char *name )
{
    if ( !wgpu.initialized ) return 0;
    return (qhandle_t)WR_RegisterShader( name, WGPU_MAT_BLEND );
}

static qhandle_t RE_RegisterShaderLightMap( const char *name, int lightmap )
{
    (void)lightmap;
    if ( !wgpu.initialized ) return 0;
    return (qhandle_t)WR_RegisterShader( name, 0 );
}

static void RE_LoadWorldMap( const char *name )
{
    if ( wgpu.initialized )
        WR_LoadWorld( name );
}
static void RE_SetWorldVisData( const byte *vis )     { (void)vis;  }
static void RE_EndRegistration( void )                {}

static void RE_BeginFrame( stereoFrame_t stereoFrame, qboolean recordingVideo )
{
    (void)stereoFrame;
    (void)recordingVideo;
    WR_BeginFrame();
}

static void RE_EndFrame( int *frontEndMsec, int *backEndMsec )
{
    if ( frontEndMsec ) *frontEndMsec = 0;
    if ( backEndMsec  ) *backEndMsec  = 0;
    WR_EndFrame();
}

static int R_MarkFragments( int numPoints, const vec3_t *points,
                              const vec3_t projection, int maxPoints,
                              vec3_t pointBuffer, int maxFragments,
                              markFragment_t *fragmentBuffer )
{
    (void)numPoints; (void)points; (void)projection;
    (void)maxPoints; (void)pointBuffer; (void)maxFragments; (void)fragmentBuffer;
    return 0;
}

static int R_LerpTag( orientation_t *tag, qhandle_t model,
                       int startFrame, int endFrame,
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

static void RE_ClearScene( void )
{
    wgpu.numEntities3D = 0;
    wgpu.numPolys3D    = 0;
}

static void RE_AddRefEntityToScene( const refEntity_t *re )
{
    wgpuEntity3D_t *e;

    if ( !wgpu.initialized || !re ) return;
    if ( re->reType != RT_MODEL ) return;
    if ( wgpu.numEntities3D >= WGPU_MAX_ENTITIES3D ) return;

    e = &wgpu.entities3D[wgpu.numEntities3D++];
    /* modelIndex: handle - 1 (handle 0 = invalid → modelIndex -1) */
    e->modelIndex = ( re->hModel > 0 ) ? (int)( re->hModel - 1 ) : -1;
    e->frame      = re->frame;
    e->oldframe   = re->oldframe;
    e->backlerp   = re->backlerp;
    VectorCopy( re->origin, e->origin );
    VectorCopy( re->axis[0], e->axis[0] );
    VectorCopy( re->axis[1], e->axis[1] );
    VectorCopy( re->axis[2], e->axis[2] );
    e->color[0] = re->shaderRGBA[0] / 255.0f;
    e->color[1] = re->shaderRGBA[1] / 255.0f;
    e->color[2] = re->shaderRGBA[2] / 255.0f;
    e->color[3] = re->shaderRGBA[3] / 255.0f;
    /* Ensure at least some colour */
    if ( e->color[0] == 0 && e->color[1] == 0 && e->color[2] == 0 && e->color[3] == 0 )
        e->color[0] = e->color[1] = e->color[2] = e->color[3] = 1.0f;
}

static void RE_AddRefEntityPtrToScene( refEntity_t *re ) { RE_AddRefEntityToScene( re ); }
static void RE_SetPathLines( int *numCameraPoints, cameraPoint_t *cameraPoints,
                              int *numSplinePoints, vec3_t *splinePoints,
                              const vec4_t color )
{
    (void)numCameraPoints; (void)cameraPoints;
    (void)numSplinePoints; (void)splinePoints; (void)color;
}
static void RE_AddPolyToScene( qhandle_t hShader, int numVerts,
                                const polyVert_t *verts, int num, int lightmap )
{
    int p, v;
    (void)lightmap;

    if ( !wgpu.initialized || !verts || numVerts < 3 ) return;

    for ( p = 0; p < num && wgpu.numPolys3D < WGPU_MAX_POLYS3D; p++ )
    {
        wgpuPoly3D_t *poly = &wgpu.polys3D[wgpu.numPolys3D++];
        int           nv   = numVerts > WGPU_MAX_POLY_VERTS ? WGPU_MAX_POLY_VERTS : numVerts;

        poly->matIndex = (int)hShader;
        poly->numVerts = nv;
        for ( v = 0; v < nv; v++ )
            poly->verts[v] = verts[p * numVerts + v];
    }
}
static int R_LightForPoint( const vec3_t point, vec3_t ambientLight,
                              vec3_t directedLight, vec3_t lightDir )
{
    (void)point;
    VectorClear( ambientLight );
    VectorClear( directedLight );
    VectorClear( lightDir );
    return 0;
}
static void RE_AddLightToScene( const vec3_t org, float intensity,
                                 float r, float g, float b )
{
    (void)org; (void)intensity; (void)r; (void)g; (void)b;
}
static void RE_AddAdditiveLightToScene( const vec3_t org, float intensity,
                                         float r, float g, float b )
{
    (void)org; (void)intensity; (void)r; (void)g; (void)b;
}
static void RE_RenderScene( const refdef_t *fd )
{
    if ( !wgpu.initialized || !fd ) return;

    /* Store viewport */
    wgpu.view3Dx = fd->x;
    wgpu.view3Dy = fd->y;
    wgpu.view3Dw = fd->width;
    wgpu.view3Dh = fd->height;

    /* Build view-projection matrix for this frame */
    WR_BuildViewProj( wgpu.viewProj, fd );
    wgpu.haveRefdef = qtrue;
}

static void RE_SetColor( const float *rgba )
{
    if ( rgba )
    {
        wgpu.color2D[0] = rgba[0];
        wgpu.color2D[1] = rgba[1];
        wgpu.color2D[2] = rgba[2];
        wgpu.color2D[3] = rgba[3];
    }
    else
    {
        wgpu.color2D[0] = wgpu.color2D[1] = wgpu.color2D[2] = wgpu.color2D[3] = 1.0f;
    }
}

static void RE_StretchPic( float x, float y, float w, float h,
                            float s1, float t1, float s2, float t2,
                            qhandle_t hShader )
{
    WR_AddStretchPic( x, y, w, h, s1, t1, s2, t2, (int)hShader );
}

static void RE_StretchRaw( int x, int y, int w, int h, int cols, int rows,
                            const byte *data, int client, qboolean dirty )
{
    /* Upload raw RGBA data as a scratch texture and draw it */
    char   scratchName[32];
    int    matIdx;
    int    imgIdx;
    byte  *rgba;
    int    i, j;

    (void)client; (void)dirty;

    if ( !wgpu.initialized || !data || cols <= 0 || rows <= 0 )
        return;

    /* Q3 cinematic data arrives as RGB rows; convert to RGBA */
    rgba = (byte *)ri.Hunk_AllocateTempMemory( cols * rows * 4 );
    if ( !rgba )
        return;

    for ( i = 0; i < rows; i++ )
    {
        for ( j = 0; j < cols; j++ )
        {
            const byte *src = data + ( i * cols + j ) * 4;
            byte       *dst = rgba  + ( i * cols + j ) * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
        }
    }

    Com_sprintf( scratchName, sizeof( scratchName ), "_scratch_%d", client );
    imgIdx = WR_CreateImageFromData( scratchName, rgba, cols, rows, qfalse, qtrue );
    ri.Hunk_FreeTempMemory( rgba );

    matIdx = WR_RegisterShaderFromData( scratchName, NULL, 0, 0, qfalse, qtrue );
    /* Update the material's imageIndex directly */
    if ( matIdx > 0 && matIdx < wgpu.numMats )
        wgpu.mats[matIdx].imageIndex = imgIdx;

    WR_AddStretchPic( (float)x, (float)y, (float)w, (float)h,
                       0.0f, 0.0f, 1.0f, 1.0f, matIdx );
}

static void RE_UploadCinematic( int w, int h, int cols, int rows,
                                 const byte *data, int client, qboolean dirty )
{
    (void)w; (void)h;
    RE_StretchRaw( 0, 0, cols, rows, cols, rows, data, client, dirty );
}

static void RE_RegisterFont( const char *fontName, int pointSize, fontInfo_t *font )
{
    (void)fontName; (void)pointSize;
    Com_Memset( font, 0, sizeof( *font ) );
}

static qboolean RE_GetGlyphInfo( fontInfo_t *fontInfo, int charValue, glyphInfo_t *glyphOut )
{
    (void)fontInfo; (void)charValue;
    Com_Memset( glyphOut, 0, sizeof( *glyphOut ) );
    return qfalse;
}

static qboolean RE_GetFontInfo( int fontId, fontInfo_t *font )
{
    (void)fontId;
    Com_Memset( font, 0, sizeof( *font ) );
    return qfalse;
}

static void R_RemapShader( const char *oldShader, const char *newShader,
                            const char *offsetTime, qboolean keepLightmap,
                            qboolean userSet )
{
    (void)oldShader; (void)newShader; (void)offsetTime;
    (void)keepLightmap; (void)userSet;
}

static void     R_ClearRemappedShader( const char *shaderName ) { (void)shaderName; }
static qboolean R_GetEntityToken( char *buffer, int size )
{
    if ( size > 0 ) buffer[0] = '\0';
    return qfalse;
}
static qboolean R_inPVS( const vec3_t p1, const vec3_t p2 )    { (void)p1; (void)p2; return qtrue; }

static void RE_TakeVideoFrame( aviFileData_t *afd, int h, int w,
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

static void RE_Get_Advertisements( int *num, float *verts, char shaders[][MAX_QPATH] )
{
    (void)verts; (void)shaders;
    if ( num ) *num = 0;
}

static void RE_ReplaceShaderImage( qhandle_t h, const ubyte *data, int width, int height )
{
    int      matIdx = (int)h;
    int      imgIdx;
    wgpuMat_t *mat;
    char      name[MAX_QPATH];

    if ( !wgpu.initialized || !data || width <= 0 || height <= 0 )
        return;

    if ( matIdx < 0 || matIdx >= wgpu.numMats )
        return;

    mat = &wgpu.mats[matIdx];
    /* Re-create the image (same name = reuse slot) */
    imgIdx = WR_CreateImageFromData( mat->name, data, width, height,
                                      qfalse, qtrue );
    mat->imageIndex = imgIdx;
}

static qhandle_t RE_RegisterShaderFromData( const char *name, ubyte *data,
                                             int width, int height,
                                             qboolean mipmap, qboolean allowPicmip,
                                             int wrapClampMode, int lightmapIndex )
{
    (void)allowPicmip; (void)lightmapIndex;
    if ( !wgpu.initialized ) return 0;
    return (qhandle_t)WR_RegisterShaderFromData(
        name, data, width, height,
        mipmap, ( wrapClampMode != 0 ) );
}

static void RE_GetShaderImageDimensions( qhandle_t h, int *width, int *height )
{
    int matIdx = (int)h;
    if ( matIdx >= 0 && matIdx < wgpu.numMats )
    {
        int imgIdx = wgpu.mats[matIdx].imageIndex;
        if ( imgIdx >= 0 && imgIdx < wgpu.numImages )
        {
            if ( width  ) *width  = wgpu.images[imgIdx].width;
            if ( height ) *height = wgpu.images[imgIdx].height;
            return;
        }
    }
    if ( width  ) *width  = 0;
    if ( height ) *height = 0;
}

static void RE_GetShaderImageData( qhandle_t h, ubyte *data )
{
    /* Cannot readback GPU textures efficiently; leave blank */
    (void)h; (void)data;
}

static qhandle_t RE_GetSingleShader( void )
{
    return 0;
}

/* =========================================================================
 * GetRefAPI – the single exported symbol
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
