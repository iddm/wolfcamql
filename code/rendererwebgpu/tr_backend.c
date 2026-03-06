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
 *   WR_BeginFrame  – acquire swap-chain texture, create command encoder
 *   WR_EndFrame    – flush 2D batch, end render pass, submit, present
 *   WR_AddStretchPic – queue one 2D quad draw
 *
 * The 3D render pass (RenderScene) currently clears the viewport only;
 * full 3D geometry support is left for a future pass.
 */

#ifdef USE_WEBGPU

#include "tr_local.h"

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/*
 * AllocVerts2D – reserve 'n' consecutive slots in the per-frame 2D vertex
 * staging array.  Returns the base index or -1 if there is no room.
 */
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

/* =========================================================================
 * WR_AddStretchPic
 *
 * Converts one (x,y,w,h,s1,t1,s2,t2) quad into 6 vertices (two triangles)
 * and appends a wgpuDraw2D_t record for the flush phase.
 * ========================================================================= */

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
    v[0].x = x;     v[0].y = y;     v[0].u = s1; v[0].v = t1;  v[0].r = r; v[0].g = g; v[0].b = b; v[0].a = a;
    v[1].x = x+w;   v[1].y = y;     v[1].u = s2; v[1].v = t1;  v[1].r = r; v[1].g = g; v[1].b = b; v[1].a = a;
    v[2].x = x+w;   v[2].y = y+h;   v[2].u = s2; v[2].v = t2;  v[2].r = r; v[2].g = g; v[2].b = b; v[2].a = a;

    /* Triangle 2: top-left, bottom-right, bottom-left */
    v[3].x = x;     v[3].y = y;     v[3].u = s1; v[3].v = t1;  v[3].r = r; v[3].g = g; v[3].b = b; v[3].a = a;
    v[4].x = x+w;   v[4].y = y+h;   v[4].u = s2; v[4].v = t2;  v[4].r = r; v[4].g = g; v[4].b = b; v[4].a = a;
    v[5].x = x;     v[5].y = y+h;   v[5].u = s1; v[5].v = t2;  v[5].r = r; v[5].g = g; v[5].b = b; v[5].a = a;

    d = &wgpu.draws2D[wgpu.numDraws2D++];
    d->matIndex  = matIndex;
    d->vertBase  = base;
    d->vertCount = 6;
}

/* =========================================================================
 * WR_BeginFrame
 *
 * Called at the start of each frame.  Acquires the swap-chain texture and
 * resets the per-frame 2D batch counters.
 * ========================================================================= */

void WR_BeginFrame( void )
{
#ifdef __EMSCRIPTEN__
    WGPUSwapChainGetCurrentTextureViewProc getView;

    if ( !wgpu.initialized )
        return;

    /* Reset 2D batch */
    wgpu.numVerts2D = 0;
    wgpu.numDraws2D = 0;

    /* Acquire current swap-chain view */
    wgpu.frameView = wgpuSwapChainGetCurrentTextureView( wgpu.swapChain );
    if ( !wgpu.frameView )
    {
        ri.Printf( PRINT_WARNING, "WR_BeginFrame: no swap chain texture\n" );
        return;
    }

    /* Create command encoder for this frame */
    WGPUCommandEncoderDescriptor encDesc;
    Com_Memset( &encDesc, 0, sizeof( encDesc ) );
    encDesc.label    = "Frame encoder";
    wgpu.encoder = wgpuDeviceCreateCommandEncoder( wgpu.device, &encDesc );

    wgpu.inFrame = qtrue;
#endif
}

/* =========================================================================
 * Flush2D – encode all queued 2D draw calls into a render pass.
 * ========================================================================= */

#ifdef __EMSCRIPTEN__
static void Flush2D( WGPURenderPassEncoder pass )
{
    int                  i;
    wgpuUniforms2D_t     ub;
    wgpuPipeline_t      *curPipe    = NULL;
    wgpuBlend_t          curBlend   = WGPU_BLEND_COUNT;  /* invalid */
    int                  curMat     = -1;

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
    wgpuRenderPassEncoderSetVertexBuffer( pass, 0, wgpu.vb2D,
                                           0, WGPUSIZE_WHOLE );

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
            mat = &wgpu.mats[0];   /* default */

        blend = (mat->flags & WGPU_MAT_ADDITIVE) ? WGPU_BLEND_ADD
              : (mat->flags & WGPU_MAT_BLEND)    ? WGPU_BLEND_ALPHA
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

        /* Create and immediately set a bind group for this draw */
        bg = WR_MakeBindGroup2D( curPipe, wgpu.ub2D, tv, samp );
        wgpuRenderPassEncoderSetBindGroup( pass, 0, bg, 0, NULL );

        /* Draw 6 vertices (two triangles) */
        wgpuRenderPassEncoderDraw( pass,
                                    (uint32_t)d->vertCount,
                                    1,
                                    (uint32_t)d->vertBase,
                                    0 );

        /* Release the per-draw bind group */
        wgpuBindGroupRelease( bg );
    }
}
#endif

/* =========================================================================
 * WR_EndFrame
 *
 * Encodes and submits the 2D pass, then presents the frame.
 * ========================================================================= */

void WR_EndFrame( void )
{
#ifdef __EMSCRIPTEN__
    WGPURenderPassColorAttachment colorAtt;
    WGPURenderPassDescriptor      passDesc;
    WGPURenderPassEncoder         pass;
    WGPUCommandBuffer             cmd;
    WGPUCommandBufferDescriptor   cmdDesc;

    if ( !wgpu.initialized || !wgpu.inFrame )
        return;

    /* ------------------------------------------------------------------
     * 2D render pass
     * ------------------------------------------------------------------ */
    Com_Memset( &colorAtt, 0, sizeof( colorAtt ) );
    colorAtt.view          = wgpu.frameView;
    colorAtt.loadOp        = WGPULoadOp_Clear;
    colorAtt.storeOp       = WGPUStoreOp_Store;
    colorAtt.clearValue.r  = 0.0;
    colorAtt.clearValue.g  = 0.0;
    colorAtt.clearValue.b  = 0.0;
    colorAtt.clearValue.a  = 1.0;

    Com_Memset( &passDesc, 0, sizeof( passDesc ) );
    passDesc.label                  = "2D pass";
    passDesc.colorAttachmentCount   = 1;
    passDesc.colorAttachments       = &colorAtt;
    passDesc.depthStencilAttachment = NULL;

    pass = wgpuCommandEncoderBeginRenderPass( wgpu.encoder, &passDesc );

    Flush2D( pass );

    wgpuRenderPassEncoderEnd( pass );
    wgpuRenderPassEncoderRelease( pass );

    /* ------------------------------------------------------------------
     * Submit + present
     * ------------------------------------------------------------------ */
    Com_Memset( &cmdDesc, 0, sizeof( cmdDesc ) );
    cmdDesc.label = "Frame";
    cmd = wgpuCommandEncoderFinish( wgpu.encoder, &cmdDesc );

    wgpuQueueSubmit( wgpu.queue, 1, &cmd );
    wgpuCommandBufferRelease( cmd );
    wgpuCommandEncoderRelease( wgpu.encoder );
    wgpu.encoder = NULL;

    wgpuSwapChainPresent( wgpu.swapChain );

    wgpuTextureViewRelease( wgpu.frameView );
    wgpu.frameView = NULL;
    wgpu.inFrame   = qfalse;
#endif
}

#endif /* USE_WEBGPU */
