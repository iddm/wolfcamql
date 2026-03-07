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
 * tr_backend.c – WebGPU frame rendering.
 *
 * Implements:
 *   WR_BeginFrame       – acquire swap-chain texture, create command encoder
 *   WR_EndFrame         – 3D pass, 2D pass, submit, present
 *   WR_AddStretchPic    – queue one 2D quad draw
 *   WR_Alloc3DUniformSlot – reserve a slot in the 3D uniform buffer
 *   WR_AddDraw3D        – queue one 3D draw call
 *   WR_BuildViewProj    – compute view × projection matrix from refdef_t
 *   WR_BuildModelMvp    – compute model × VP matrix for an entity
 *   WR_MatMul           – column-major 4×4 matrix multiplication
 */

#ifdef USE_WEBGPU

#include "tr_local.h"
#include <math.h>

/* =========================================================================
 * WR_MatMul – column-major 4×4 matrix multiplication: out = a * b
 * ========================================================================= */

void WR_MatMul( float out[16], const float a[16], const float b[16] )
{
    int r, c, k;
    float tmp[16];

    for ( c = 0; c < 4; c++ )
    {
        for ( r = 0; r < 4; r++ )
        {
            float sum = 0.0f;
            for ( k = 0; k < 4; k++ )
                sum += a[k * 4 + r] * b[c * 4 + k];
            tmp[c * 4 + r] = sum;
        }
    }
    Com_Memcpy( out, tmp, sizeof( tmp ) );
}

/* =========================================================================
 * WR_BuildViewProj
 *
 * Build the view × projection matrix from a refdef_t.  The view and
 * projection setup mirrors renderergl2 exactly (same s_flipMatrix and
 * projection formulae) so the result works with our vertex shader's
 *   o.pos.z = (o.pos.z + o.pos.w) * 0.5;
 * which converts GL NDC z ∈ [-w,+w] to WebGPU depth z ∈ [0,w].
 * ========================================================================= */

void WR_BuildViewProj( float out[16], const refdef_t *fd )
{
    /* ---- View matrix (same construction as R_RotateForViewer) ---- */
    float       viewerMat[16];
    const float *fwd   = fd->viewaxis[0];
    const float *right = fd->viewaxis[1];
    const float *up    = fd->viewaxis[2];
    const float *org   = fd->vieworg;

    /* Column-major viewer matrix (the three axis rows become the first
       three columns of the transposed rotation, then negated translation) */
    viewerMat[0] = fwd[0];
    viewerMat[4] = fwd[1];
    viewerMat[8] = fwd[2];
    viewerMat[12] = -( org[0] * fwd[0]   + org[1] * fwd[1]   + org[2] * fwd[2]   );

    viewerMat[1] = right[0];
    viewerMat[5] = right[1];
    viewerMat[9] = right[2];
    viewerMat[13] = -( org[0] * right[0] + org[1] * right[1] + org[2] * right[2] );

    viewerMat[2] = up[0];
    viewerMat[6] = up[1];
    viewerMat[10] = up[2];
    viewerMat[14] = -( org[0] * up[0]    + org[1] * up[1]    + org[2] * up[2]    );

    viewerMat[3]  = 0.0f;
    viewerMat[7]  = 0.0f;
    viewerMat[11] = 0.0f;
    viewerMat[15] = 1.0f;

    /* Flip matrix: Quake looks down +X, OpenGL/WebGPU looks down -Z.
       Same as renderergl2's s_flipMatrix (column-major):
         col0: { 0,  0, -1, 0 }
         col1: { -1, 0,  0, 0 }
         col2: { 0,  1,  0, 0 }
         col3: { 0,  0,  0, 1 }                                         */
    static const float flipMat[16] = {
         0,  0, -1, 0,
        -1,  0,  0, 0,
         0,  1,  0, 0,
         0,  0,  0, 1
    };

    float viewMat[16];
    WR_MatMul( viewMat, viewerMat, flipMat );

    /* ---- Projection matrix (R_SetupProjection + R_SetupProjectionZ) ---- */
    float projMat[16];
    float fovX  = fd->fov_x;
    float fovY  = fd->fov_y;
    float zNear = 4.0f;
    float zFar  = 65536.0f;

    float ymax  = zNear * (float)tan( fovY * M_PI / 360.0 );
    float xmax  = zNear * (float)tan( fovX * M_PI / 360.0 );
    float width = 2.0f * xmax;
    float height= 2.0f * ymax;
    float depth = zFar - zNear;

    Com_Memset( projMat, 0, sizeof( projMat ) );

    /* Column-major OpenGL perspective (looking down -Z, depth GL [-1,1]).
       The vertex shader remaps z to WebGPU [0,1]. */
    projMat[0]  = 2.0f * zNear / width;
    projMat[5]  = 2.0f * zNear / height;
    projMat[10] = -( zFar + zNear ) / depth;
    projMat[11] = -1.0f;
    projMat[14] = -2.0f * zFar * zNear / depth;

    /* ---- Combine: VP = proj * view ---- */
    WR_MatMul( out, projMat, viewMat );
}

/* =========================================================================
 * WR_BuildModelMvp
 *
 * Build MVP = viewProj * modelMatrix for an entity.
 * axis[0] = forward, axis[1] = right, axis[2] = up (Q3 convention).
 * ========================================================================= */

void WR_BuildModelMvp( float out[16], const float vp[16],
                        const float origin[3], const float axis[3][3] )
{
    float modelMat[16];

    /* Column-major model matrix (the entity axes become columns):
       col0 = forward, col1 = right, col2 = up, col3 = translation        */
    modelMat[0]  = axis[0][0];  modelMat[1]  = axis[0][1];  modelMat[2]  = axis[0][2];  modelMat[3]  = 0;
    modelMat[4]  = axis[1][0];  modelMat[5]  = axis[1][1];  modelMat[6]  = axis[1][2];  modelMat[7]  = 0;
    modelMat[8]  = axis[2][0];  modelMat[9]  = axis[2][1];  modelMat[10] = axis[2][2];  modelMat[11] = 0;
    modelMat[12] = origin[0];   modelMat[13] = origin[1];   modelMat[14] = origin[2];   modelMat[15] = 1;

    WR_MatMul( out, vp, modelMat );
}

/* =========================================================================
 * WR_Alloc3DUniformSlot
 *
 * Reserve a 256-byte-aligned slot in the per-frame 3D uniform buffer
 * and write the MVP + colour into it on the CPU side.
 *
 * Returns the slot index (multiply by WGPU_3D_UB_STRIDE for byte offset),
 * or -1 if the buffer is full.
 * ========================================================================= */

int WR_Alloc3DUniformSlot( const float mvp[16], const float color[4] )
{
    int               slot;
    wgpuUniforms3D_t *u;

    if ( wgpu.numUB3DSlots >= WGPU_MAX_DRAWS3D )
    {
        ri.Printf( PRINT_WARNING, "WR_Alloc3DUniformSlot: buffer full\n" );
        return -1;
    }

    slot = wgpu.numUB3DSlots++;
    u    = (wgpuUniforms3D_t *)( wgpu.ub3DData + (size_t)slot * WGPU_3D_UB_STRIDE );
    Com_Memcpy( u->mvp,   mvp,   sizeof( u->mvp   ) );
    Com_Memcpy( u->color, color, sizeof( u->color ) );

    return slot;
}

/* =========================================================================
 * WR_AddDraw3D – append a 3D draw call to the per-frame list.
 * ========================================================================= */

void WR_AddDraw3D( int matIndex, int uniformSlot,
                   WGPUBuffer vb, uint64_t vbOff,
                   WGPUBuffer ib, uint64_t ibOff,
                   int numIndexes, wgpuBlend_t blend )
{
    wgpuDraw3D_t *d;

    if ( wgpu.numDraws3D >= WGPU_MAX_DRAWS3D )
    {
        ri.Printf( PRINT_WARNING, "WR_AddDraw3D: draw list full\n" );
        return;
    }

    d = &wgpu.draws3D[wgpu.numDraws3D++];
    d->matIndex    = matIndex;
    d->uniformSlot = uniformSlot;
    d->vb          = vb;
    d->vbByteOffset= vbOff;
    d->ib          = ib;
    d->ibByteOffset= ibOff;
    d->numIndexes  = numIndexes;
    d->blend       = blend;
}

/* =========================================================================
 * WR_AddStretchPic
 * ========================================================================= */

static int AllocVerts2D( int n )
{
    int base = wgpu.numVerts2D;
    if ( base + n > MAX_2D_VERTS )
    {
        ri.Printf( PRINT_WARNING,
                   "WR_Backend: 2D vertex buffer overflow (need %d, used %d)\n",
                   n, wgpu.numVerts2D );
        return -1;
    }
    wgpu.numVerts2D += n;
    return base;
}

void WR_AddStretchPic( float x, float y, float w, float h,
                        float s1, float t1, float s2, float t2,
                        int matIndex )
{
    int            base;
    wgpuVert2D_t  *v;
    wgpuDraw2D_t  *d;
    float          r, g, b, a;

    if ( !wgpu.initialized )
        return;

    if ( wgpu.numDraws2D >= WGPU_MAX_DRAWS2D )
        return;

    base = AllocVerts2D( 6 );
    if ( base < 0 )
        return;

    r = wgpu.color2D[0];
    g = wgpu.color2D[1];
    b = wgpu.color2D[2];
    a = wgpu.color2D[3];

    v = &wgpu.verts2D[base];

    /* Triangle 1: top-left, top-right, bottom-right */
    v[0].x = x;   v[0].y = y;   v[0].u = s1; v[0].v = t1;  v[0].r = r; v[0].g = g; v[0].b = b; v[0].a = a;
    v[1].x = x+w; v[1].y = y;   v[1].u = s2; v[1].v = t1;  v[1].r = r; v[1].g = g; v[1].b = b; v[1].a = a;
    v[2].x = x+w; v[2].y = y+h; v[2].u = s2; v[2].v = t2;  v[2].r = r; v[2].g = g; v[2].b = b; v[2].a = a;

    /* Triangle 2: top-left, bottom-right, bottom-left */
    v[3].x = x;   v[3].y = y;   v[3].u = s1; v[3].v = t1;  v[3].r = r; v[3].g = g; v[3].b = b; v[3].a = a;
    v[4].x = x+w; v[4].y = y+h; v[4].u = s2; v[4].v = t2;  v[4].r = r; v[4].g = g; v[4].b = b; v[4].a = a;
    v[5].x = x;   v[5].y = y+h; v[5].u = s1; v[5].v = t2;  v[5].r = r; v[5].g = g; v[5].b = b; v[5].a = a;

    d = &wgpu.draws2D[wgpu.numDraws2D++];
    d->matIndex  = matIndex;
    d->vertBase  = base;
    d->vertCount = 6;
}

/* =========================================================================
 * WR_BeginFrame
 * ========================================================================= */

void WR_BeginFrame( void )
{
#ifdef __EMSCRIPTEN__
    if ( !wgpu.initialized )
        return;

    /* Reset all per-frame counters */
    wgpu.numVerts2D      = 0;
    wgpu.numDraws2D      = 0;
    wgpu.numDraws3D      = 0;
    wgpu.numUB3DSlots    = 0;
    wgpu.numDynVerts3D   = 0;
    wgpu.numDynIndexes3D = 0;
    wgpu.numEntities3D   = 0;
    wgpu.numPolys3D      = 0;
    wgpu.haveRefdef      = qfalse;

    /* Acquire current swap-chain view */
    wgpu.frameView = wgpuSwapChainGetCurrentTextureView( wgpu.swapChain );
    if ( !wgpu.frameView )
    {
        ri.Printf( PRINT_WARNING, "WR_BeginFrame: no swap chain texture\n" );
        return;
    }

    /* Create command encoder for this frame */
    {
        WGPUCommandEncoderDescriptor encDesc;
        Com_Memset( &encDesc, 0, sizeof( encDesc ) );
        encDesc.label = "Frame encoder";
        wgpu.encoder  = wgpuDeviceCreateCommandEncoder( wgpu.device, &encDesc );
    }

    wgpu.inFrame = qtrue;
#endif
}

/* =========================================================================
 * Flush3D – encode all queued 3D draw calls into the current render pass.
 * ========================================================================= */

#ifdef __EMSCRIPTEN__
static void Flush3D( WGPURenderPassEncoder pass )
{
    int              i;
    wgpuPipeline_t  *curPipe  = NULL;
    wgpuBlend_t      curBlend = WGPU_BLEND_COUNT;  /* invalid sentinel */
    WGPUBuffer       curVB    = NULL;
    WGPUBuffer       curIB    = NULL;

    if ( wgpu.numDraws3D == 0 )
        return;

    /* Upload all 3D uniforms at once */
    if ( wgpu.numUB3DSlots > 0 )
    {
        wgpuQueueWriteBuffer( wgpu.queue, wgpu.ub3D, 0,
                               wgpu.ub3DData,
                               (size_t)wgpu.numUB3DSlots * WGPU_3D_UB_STRIDE );
    }

    /* Upload dynamic vertex data */
    if ( wgpu.numDynVerts3D > 0 )
    {
        wgpuQueueWriteBuffer( wgpu.queue, wgpu.dynVB3D, 0,
                               wgpu.dynVerts3D,
                               (size_t)wgpu.numDynVerts3D * sizeof( wgpuVert3D_t ) );
    }

    /* Upload dynamic index data */
    if ( wgpu.numDynIndexes3D > 0 )
    {
        wgpuQueueWriteBuffer( wgpu.queue, wgpu.dynIB3D, 0,
                               wgpu.dynIndexes3D,
                               (size_t)wgpu.numDynIndexes3D * sizeof( int ) );
    }

    for ( i = 0; i < wgpu.numDraws3D; i++ )
    {
        const wgpuDraw3D_t *d = &wgpu.draws3D[i];
        wgpuImage_t        *img;
        WGPUTextureView     tv;
        WGPUSampler         samp;
        WGPUBindGroup       bg;
        uint64_t            ubOffset;
        int                 imgIdx;

        /* Switch pipeline if blend mode changed */
        if ( d->blend != curBlend )
        {
            curPipe  = WR_GetPipeline3D( d->blend );
            curBlend = d->blend;
            wgpuRenderPassEncoderSetPipeline( pass, curPipe->pipeline );
            curVB = NULL;  /* force vertex/index buffer rebind */
            curIB = NULL;
        }

        /* Resolve image */
        if ( d->matIndex >= 0 && d->matIndex < wgpu.numMats )
            imgIdx = wgpu.mats[d->matIndex].imageIndex;
        else
            imgIdx = wgpu.whiteImageIdx;

        if ( imgIdx < 0 || imgIdx >= wgpu.numImages )
            imgIdx = wgpu.whiteImageIdx;

        img  = &wgpu.images[imgIdx];
        tv   = img->view    ? img->view    : wgpu.images[wgpu.whiteImageIdx].view;
        samp = img->sampler ? img->sampler : wgpu.images[wgpu.whiteImageIdx].sampler;

        /* Bind group (per draw – released at end) */
        ubOffset = (uint64_t)d->uniformSlot * WGPU_3D_UB_STRIDE;
        bg = WR_MakeBindGroup3D( curPipe, wgpu.ub3D, ubOffset, tv, samp );
        wgpuRenderPassEncoderSetBindGroup( pass, 0, bg, 0, NULL );

        /* Vertex buffer (skip if same as last draw) */
        if ( d->vb != curVB )
        {
            wgpuRenderPassEncoderSetVertexBuffer( pass, 0, d->vb, 0, WGPUSIZE_WHOLE );
            curVB = d->vb;
        }

        /* Index buffer (skip if same as last draw) */
        if ( d->ib != curIB )
        {
            wgpuRenderPassEncoderSetIndexBuffer( pass, d->ib,
                                                  WGPUIndexFormat_Uint32,
                                                  0, WGPUSIZE_WHOLE );
            curIB = d->ib;
        }

        /* Draw */
        wgpuRenderPassEncoderDrawIndexed( pass,
                                           (uint32_t)d->numIndexes,
                                           1,
                                           (uint32_t)( d->ibByteOffset / sizeof( int ) ),
                                           0,
                                           0 );

        wgpuBindGroupRelease( bg );
    }
}
#endif

/* =========================================================================
 * Flush2D – encode all queued 2D draw calls into the current render pass.
 * ========================================================================= */

#ifdef __EMSCRIPTEN__
static void Flush2D( WGPURenderPassEncoder pass )
{
    int                  i;
    wgpuUniforms2D_t     ub;
    wgpuPipeline_t      *curPipe  = NULL;
    wgpuBlend_t          curBlend = WGPU_BLEND_COUNT;

    if ( wgpu.numDraws2D == 0 || wgpu.numVerts2D == 0 )
        return;

    /* Upload vertex data */
    wgpuQueueWriteBuffer( wgpu.queue, wgpu.vb2D, 0,
                           wgpu.verts2D,
                           (size_t)( wgpu.numVerts2D * sizeof( wgpuVert2D_t ) ) );

    /* Upload 2D uniforms (screen dimensions) */
    ub.screenW = (float)wgpu.vidWidth;
    ub.screenH = (float)wgpu.vidHeight;
    ub._pad[0] = ub._pad[1] = 0.0f;
    wgpuQueueWriteBuffer( wgpu.queue, wgpu.ub2D, 0, &ub, sizeof( ub ) );

    /* Set vertex buffer once – all 2D draws share the same VBO */
    wgpuRenderPassEncoderSetVertexBuffer( pass, 0, wgpu.vb2D, 0, WGPUSIZE_WHOLE );

    for ( i = 0; i < wgpu.numDraws2D; i++ )
    {
        const wgpuDraw2D_t *d   = &wgpu.draws2D[i];
        const wgpuMat_t    *mat;
        wgpuBlend_t         blend;
        wgpuImage_t        *img;
        WGPUBindGroup       bg;
        WGPUTextureView     tv;
        WGPUSampler         samp;
        int                 imgIdx;

        /* Resolve material */
        if ( d->matIndex >= 0 && d->matIndex < wgpu.numMats )
            mat = &wgpu.mats[d->matIndex];
        else
            mat = &wgpu.mats[0];

        blend = (mat->flags & WGPU_MAT_ADDITIVE) ? WGPU_BLEND_ADD
              : WGPU_BLEND_ALPHA;   /* default to alpha for 2D */

        /* Switch pipeline if blend mode changed */
        if ( blend != curBlend )
        {
            curPipe  = WR_GetPipeline2D( blend );
            curBlend = blend;
            wgpuRenderPassEncoderSetPipeline( pass, curPipe->pipeline );
        }

        /* Resolve image */
        imgIdx = ( mat->imageIndex >= 0 && mat->imageIndex < wgpu.numImages )
                ? mat->imageIndex : wgpu.whiteImageIdx;
        img    = &wgpu.images[imgIdx];

        tv   = img->view    ? img->view    : wgpu.images[wgpu.whiteImageIdx].view;
        samp = img->sampler ? img->sampler : wgpu.images[wgpu.whiteImageIdx].sampler;

        bg = WR_MakeBindGroup2D( curPipe, wgpu.ub2D, tv, samp );
        wgpuRenderPassEncoderSetBindGroup( pass, 0, bg, 0, NULL );

        wgpuRenderPassEncoderDraw( pass,
                                    (uint32_t)d->vertCount,
                                    1,
                                    (uint32_t)d->vertBase,
                                    0 );
        wgpuBindGroupRelease( bg );
    }
}
#endif

/* =========================================================================
 * WR_EndFrame
 *
 * 1. Build entity / poly draws from the queued scene objects.
 * 2. 3D render pass: clear + depth, render world + entities.
 * 3. 2D render pass: load (keep 3D output), render HUD / menus.
 * 4. Submit + present.
 * ========================================================================= */

void WR_EndFrame( void )
{
#ifdef __EMSCRIPTEN__
    if ( !wgpu.initialized || !wgpu.inFrame )
        return;

    /* ---- Build scene draws (entities + polys) ---------------------- */
    if ( wgpu.haveRefdef )
    {
        int e;

        /* World geometry */
        WR_AddWorldToFrame();

        /* Entities */
        for ( e = 0; e < wgpu.numEntities3D; e++ )
            WR_AddEntityToFrame( &wgpu.entities3D[e] );

        /* Polys */
        WR_AddPolysToFrame();
    }

    /* ------------------------------------------------------------------
     * 3D render pass – clear colour + depth, then render world/entities
     * ------------------------------------------------------------------ */
    {
        WGPURenderPassColorAttachment   colorAtt;
        WGPURenderPassDepthStencilAttachment depthAtt;
        WGPURenderPassDescriptor        passDesc;
        WGPURenderPassEncoder           pass;

        Com_Memset( &colorAtt, 0, sizeof( colorAtt ) );
        colorAtt.view          = wgpu.frameView;
        colorAtt.loadOp        = WGPULoadOp_Clear;
        colorAtt.storeOp       = WGPUStoreOp_Store;
        colorAtt.clearValue.r  = 0.0;
        colorAtt.clearValue.g  = 0.0;
        colorAtt.clearValue.b  = 0.0;
        colorAtt.clearValue.a  = 1.0;

        Com_Memset( &depthAtt, 0, sizeof( depthAtt ) );
        depthAtt.view              = wgpu.depthView;
        depthAtt.depthLoadOp       = WGPULoadOp_Clear;
        depthAtt.depthStoreOp      = WGPUStoreOp_Store;
        depthAtt.depthClearValue   = 1.0f;
        depthAtt.stencilLoadOp     = WGPULoadOp_Clear;
        depthAtt.stencilStoreOp    = WGPUStoreOp_Discard;
        depthAtt.stencilClearValue = 0;

        Com_Memset( &passDesc, 0, sizeof( passDesc ) );
        passDesc.label                  = "3D pass";
        passDesc.colorAttachmentCount   = 1;
        passDesc.colorAttachments       = &colorAtt;
        passDesc.depthStencilAttachment = &depthAtt;

        pass = wgpuCommandEncoderBeginRenderPass( wgpu.encoder, &passDesc );
        Flush3D( pass );
        wgpuRenderPassEncoderEnd( pass );
        wgpuRenderPassEncoderRelease( pass );
    }

    /* ------------------------------------------------------------------
     * 2D render pass – load colour (keep 3D output), no depth
     * ------------------------------------------------------------------ */
    {
        WGPURenderPassColorAttachment colorAtt;
        WGPURenderPassDescriptor      passDesc;
        WGPURenderPassEncoder         pass;

        Com_Memset( &colorAtt, 0, sizeof( colorAtt ) );
        colorAtt.view    = wgpu.frameView;
        colorAtt.loadOp  = WGPULoadOp_Load;   /* keep 3D render output */
        colorAtt.storeOp = WGPUStoreOp_Store;

        Com_Memset( &passDesc, 0, sizeof( passDesc ) );
        passDesc.label                  = "2D pass";
        passDesc.colorAttachmentCount   = 1;
        passDesc.colorAttachments       = &colorAtt;
        passDesc.depthStencilAttachment = NULL;

        pass = wgpuCommandEncoderBeginRenderPass( wgpu.encoder, &passDesc );
        Flush2D( pass );
        wgpuRenderPassEncoderEnd( pass );
        wgpuRenderPassEncoderRelease( pass );
    }

    /* ------------------------------------------------------------------
     * Submit + present
     * ------------------------------------------------------------------ */
    {
        WGPUCommandBufferDescriptor cmdDesc;
        WGPUCommandBuffer           cmd;

        Com_Memset( &cmdDesc, 0, sizeof( cmdDesc ) );
        cmdDesc.label = "Frame";
        cmd = wgpuCommandEncoderFinish( wgpu.encoder, &cmdDesc );

        wgpuQueueSubmit( wgpu.queue, 1, &cmd );
        wgpuCommandBufferRelease( cmd );
        wgpuCommandEncoderRelease( wgpu.encoder );
        wgpu.encoder = NULL;
    }

    wgpuSwapChainPresent( wgpu.swapChain );

    wgpuTextureViewRelease( wgpu.frameView );
    wgpu.frameView = NULL;
    wgpu.inFrame   = qfalse;
#endif
}

#endif /* USE_WEBGPU */
