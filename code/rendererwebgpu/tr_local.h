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
 * tr_local.h – WebGPU renderer internal types and globals.
 *
 * This header is compiled only when USE_WEBGPU=1.  It replaces the
 * renderergl2/tr_local.h for all WebGPU-specific renderer files.
 */

#ifndef TR_WEBGPU_LOCAL_H
#define TR_WEBGPU_LOCAL_H

#ifdef USE_WEBGPU

#include "../qcommon/q_shared.h"
#include "../qcommon/qfiles.h"
#include "../renderercommon/tr_public.h"

/* Pull in our stub qgl.h so that renderercommon headers (tr_common.h)
   compile without the real OpenGL includes. */
#include "qgl.h"

/* renderercommon shared header (image_t, imgType_t, etc.) */
#include "../renderercommon/tr_common.h"

#ifdef __EMSCRIPTEN__
#include <SDL.h>
#include <emscripten/html5_webgpu.h>
#include <webgpu/webgpu.h>
#endif

/* =========================================================================
 * Limits
 * ========================================================================= */

#define MAX_WEBGPU_IMAGES       2048
#define MAX_WEBGPU_MATS         16384
#define MAX_2D_VERTS            (65536 * 6)  /* 6 verts per quad */
#define WGPU_2D_VB_SIZE         (MAX_2D_VERTS * 32)  /* 32 bytes per vertex */
#define WGPU_UNIFORM_BUF_SIZE   256          /* padded to 256-byte alignment */
#define MAX_PIPELINE_CACHE      16

/* =========================================================================
 * WebGPU image slot
 *
 * Extends the renderercommon image_t (which has imgName, width, height,
 * texnum for GL) with the WebGPU-specific resources.
 * ========================================================================= */

typedef struct {
    char            name[MAX_QPATH];
    int             width;
    int             height;
    qboolean        hasAlpha;       /* has any transparent pixels */
    /* WebGPU objects */
    WGPUTexture     texture;
    WGPUTextureView view;
    WGPUSampler     sampler;
} wgpuImage_t;

/* =========================================================================
 * WebGPU material (shader) slot – simplified flat material
 * ========================================================================= */

#define WGPU_MAT_BLEND   0x01   /* uses alpha blending */
#define WGPU_MAT_ADDITIVE 0x02  /* additive blending */

typedef struct {
    char        name[MAX_QPATH];
    int         imageIndex;   /* index into wgpu.images[], -1 = white */
    int         flags;        /* WGPU_MAT_* bitmask */
    float       sort;         /* draw-order hint */
} wgpuMat_t;

/* =========================================================================
 * 2D vertex layout (interleaved, 32 bytes)
 *   offset  0: float x, y       (screen pixels)
 *   offset  8: float u, v       (texture coordinates)
 *   offset 16: float r, g, b, a (color modulate)
 * ========================================================================= */

typedef struct {
    float x, y;
    float u, v;
    float r, g, b, a;
} wgpuVert2D_t;

/* =========================================================================
 * Uniform buffer for the 2D pass (written once per frame)
 * ========================================================================= */

typedef struct {
    float screenW;
    float screenH;
    float _pad[2];
} wgpuUniforms2D_t;

/* =========================================================================
 * Blend modes for the pipeline cache
 * ========================================================================= */

typedef enum {
    WGPU_BLEND_OPAQUE = 0,
    WGPU_BLEND_ALPHA,
    WGPU_BLEND_ADD,
    WGPU_BLEND_COUNT
} wgpuBlend_t;

/* =========================================================================
 * Cached render pipeline
 * ========================================================================= */

typedef struct {
    WGPURenderPipeline  pipeline;
    WGPUBindGroupLayout bgl;         /* bind group layout (uniforms + texture) */
    WGPUPipelineLayout  layout;
    wgpuBlend_t         blend;
    qboolean            valid;
} wgpuPipeline_t;

/* =========================================================================
 * Queued 2D draw call (sorted by shaderIndex to batch textures)
 * ========================================================================= */

typedef struct {
    int matIndex;
    int vertBase;   /* index into wgpu.verts2D[] */
    int vertCount;  /* always 6 for a quad */
} wgpuDraw2D_t;

/* =========================================================================
 * Global WebGPU renderer state
 * ========================================================================= */

#define WGPU_MAX_DRAWS2D  32768

typedef struct {
    /* ---- Window / device -------------------------------------------- */
    SDL_Window      *window;
    int              vidWidth;
    int              vidHeight;

    WGPUDevice       device;
    WGPUQueue        queue;
    WGPUSurface      surface;
    WGPUSwapChain    swapChain;

    /* Depth/stencil texture */
    WGPUTexture      depthTex;
    WGPUTextureView  depthView;

    /* ---- Shader modules --------------------------------------------- */
    WGPUShaderModule shaderMod2D;   /* compiled WGSL for 2D */
    WGPUShaderModule shaderMod3D;   /* compiled WGSL for 3D */

    /* ---- Pipeline cache --------------------------------------------- */
    wgpuPipeline_t   pipe2D[WGPU_BLEND_COUNT];
    wgpuPipeline_t   pipe3D[WGPU_BLEND_COUNT];

    /* ---- Images ----------------------------------------------------- */
    wgpuImage_t      images[MAX_WEBGPU_IMAGES];
    int              numImages;
    int              whiteImageIdx; /* 1×1 solid white fallback */

    /* ---- Materials -------------------------------------------------- */
    wgpuMat_t        mats[MAX_WEBGPU_MATS];
    int              numMats;

    /* ---- Per-frame 2D batch ----------------------------------------- */
    WGPUBuffer       vb2D;          /* GPU vertex buffer (mapped write) */
    WGPUBuffer       ub2D;          /* GPU uniform buffer */
    wgpuVert2D_t    *verts2D;       /* CPU staging array */
    int              numVerts2D;    /* vertices written this frame */
    wgpuDraw2D_t     draws2D[WGPU_MAX_DRAWS2D];
    int              numDraws2D;

    /* ---- Current 2D draw colour ------------------------------------- */
    float            color2D[4];    /* set by RE_SetColor */

    /* ---- Per-frame encoding state ----------------------------------- */
    WGPUTextureView    frameView;   /* swap chain texture view */
    WGPUCommandEncoder encoder;
    qboolean           inFrame;

    qboolean           initialized;
} wgpuGlobals_t;

/* =========================================================================
 * Extern globals (defined in tr_init.c)
 * ========================================================================= */

extern wgpuGlobals_t  wgpu;
extern refimport_t    ri;
extern glconfig_t     glConfig;

/* =========================================================================
 * Function prototypes
 * ========================================================================= */

/* tr_image.c */
int  WR_FindImage( const char *name );
int  WR_CreateImageFromData( const char *name, const byte *rgba,
                              int width, int height,
                              qboolean mipmap, qboolean clampToEdge );
int  WR_LoadImage( const char *name );
void WR_InitImages( void );
void WR_FreeImages( void );

/* tr_shader.c */
void WR_InitShaders( void );
int  WR_FindShader( const char *name );
int  WR_RegisterShader( const char *name, int flags );
int  WR_RegisterShaderFromData( const char *name, const byte *rgba,
                                 int width, int height,
                                 qboolean mipmap, qboolean clampToEdge );

/* tr_wgsl.c */
void            WR_InitShaderModules( void );
void            WR_InitPipelines( void );
wgpuPipeline_t *WR_GetPipeline2D( wgpuBlend_t blend );
WGPUBindGroup   WR_MakeBindGroup2D( const wgpuPipeline_t *pipe,
                                     WGPUBuffer ub,
                                     WGPUTextureView tv,
                                     WGPUSampler samp );

/* tr_backend.c */
void WR_BeginFrame( void );
void WR_EndFrame( void );
void WR_AddStretchPic( float x, float y, float w, float h,
                        float s1, float t1, float s2, float t2,
                        int matIndex );

/* tr_init.c (platform / window) */
void GLimp_Init( qboolean fixedFunction );
void GLimp_Shutdown( void );
void GLimp_Minimize( void );
void GLimp_EndFrame( void );
void GLimp_LogComment( char *comment );
void GLimp_InitExtraExtensions( void );

#endif /* USE_WEBGPU */
#endif /* TR_WEBGPU_LOCAL_H */
