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
 * tr_wgsl.c – WGSL shader sources, pipeline and bind-group management.
 *
 * Two sets of shaders / pipelines:
 *   2D – for RE_StretchPic / HUD / menus (no depth, alpha-blended or opaque)
 *   3D – for world / entity geometry    (depth test, opaque or alpha-tested)
 *
 * Pipelines are cached by blend mode.  Bind groups are created per draw call
 * and released immediately after submission (they are ref-counted by WebGPU).
 */

#ifdef USE_WEBGPU

#include "tr_local.h"

/* =========================================================================
 * WGSL source: 2D pass
 *
 * Vertex layout (stride 32 bytes):
 *   location 0: vec2<f32>  x, y   (screen pixels)
 *   location 1: vec2<f32>  u, v   (texture coords)
 *   location 2: vec4<f32>  r,g,b,a (colour modulate)
 *
 * Bind group 0:
 *   binding 0 – uniform buffer { screenW, screenH, pad, pad }
 *   binding 1 – texture_2d<f32>
 *   binding 2 – sampler
 * ========================================================================= */

static const char *k_wgsl2D =
    "struct Uniforms2D {\n"
    "  screenW : f32,\n"
    "  screenH : f32,\n"
    "  pad0    : f32,\n"
    "  pad1    : f32,\n"
    "};\n"
    "\n"
    "@group(0) @binding(0) var<uniform> u : Uniforms2D;\n"
    "@group(0) @binding(1) var tex : texture_2d<f32>;\n"
    "@group(0) @binding(2) var smp : sampler;\n"
    "\n"
    "struct VIn {\n"
    "  @location(0) pos : vec2<f32>,\n"
    "  @location(1) uv  : vec2<f32>,\n"
    "  @location(2) col : vec4<f32>,\n"
    "};\n"
    "\n"
    "struct VOut {\n"
    "  @builtin(position) pos : vec4<f32>,\n"
    "  @location(0)       uv  : vec2<f32>,\n"
    "  @location(1)       col : vec4<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(v : VIn) -> VOut {\n"
    "  var o : VOut;\n"
    "  let ndcX =  (v.pos.x / u.screenW) * 2.0 - 1.0;\n"
    "  let ndcY = -(v.pos.y / u.screenH) * 2.0 + 1.0;\n"
    "  o.pos = vec4<f32>(ndcX, ndcY, 0.0, 1.0);\n"
    "  o.uv  = v.uv;\n"
    "  o.col = v.col;\n"
    "  return o;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(v : VOut) -> @location(0) vec4<f32> {\n"
    "  return textureSample(tex, smp, v.uv) * v.col;\n"
    "}\n";

/* =========================================================================
 * WGSL source: 3D pass
 *
 * Vertex layout (stride 20 bytes):
 *   location 0: vec3<f32>  x, y, z
 *   location 1: vec2<f32>  u, v
 *
 * Bind group 0:
 *   binding 0 – uniform buffer { mvp: mat4x4<f32>, color: vec4<f32> }
 *   binding 1 – texture_2d<f32>
 *   binding 2 – sampler
 * ========================================================================= */

static const char *k_wgsl3D =
    "struct Uniforms3D {\n"
    "  mvp   : mat4x4<f32>,\n"
    "  color : vec4<f32>,\n"
    "};\n"
    "\n"
    "@group(0) @binding(0) var<uniform> u : Uniforms3D;\n"
    "@group(0) @binding(1) var tex : texture_2d<f32>;\n"
    "@group(0) @binding(2) var smp : sampler;\n"
    "\n"
    "struct VIn {\n"
    "  @location(0) pos : vec3<f32>,\n"
    "  @location(1) uv  : vec2<f32>,\n"
    "};\n"
    "\n"
    "struct VOut {\n"
    "  @builtin(position) pos : vec4<f32>,\n"
    "  @location(0)       uv  : vec2<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(v : VIn) -> VOut {\n"
    "  var o : VOut;\n"
    "  o.pos = u.mvp * vec4<f32>(v.pos, 1.0);\n"
    "  o.uv  = v.uv;\n"
    "  return o;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(v : VOut) -> @location(0) vec4<f32> {\n"
    "  return textureSample(tex, smp, v.uv) * u.color;\n"
    "}\n";

/* =========================================================================
 * WR_InitShaderModules – compile WGSL sources into shader modules.
 * ========================================================================= */

void WR_InitShaderModules( void )
{
#ifdef __EMSCRIPTEN__
    WGPUShaderModuleWGSLDescriptor wgslDesc;
    WGPUShaderModuleDescriptor     modDesc;

    /* 2D module */
    Com_Memset( &wgslDesc, 0, sizeof( wgslDesc ) );
    wgslDesc.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
    wgslDesc.code        = k_wgsl2D;

    Com_Memset( &modDesc, 0, sizeof( modDesc ) );
    modDesc.nextInChain = &wgslDesc.chain;
    modDesc.label       = "2D shader";

    wgpu.shaderMod2D = wgpuDeviceCreateShaderModule( wgpu.device, &modDesc );
    if ( !wgpu.shaderMod2D )
        ri.Error( ERR_FATAL, "WR_InitShaderModules: failed to compile 2D WGSL" );

    /* 3D module */
    wgslDesc.code = k_wgsl3D;
    modDesc.label = "3D shader";

    wgpu.shaderMod3D = wgpuDeviceCreateShaderModule( wgpu.device, &modDesc );
    if ( !wgpu.shaderMod3D )
        ri.Error( ERR_FATAL, "WR_InitShaderModules: failed to compile 3D WGSL" );
#endif
}

/* =========================================================================
 * BuildBindGroupLayout2D – creates the BGL for the 2D pipeline:
 *   binding 0: uniform buffer (vertex)
 *   binding 1: texture_2d     (fragment)
 *   binding 2: sampler        (fragment)
 * ========================================================================= */

#ifdef __EMSCRIPTEN__
static WGPUBindGroupLayout BuildBindGroupLayout2D( void )
{
    WGPUBindGroupLayoutEntry entries[3];
    WGPUBindGroupLayoutDescriptor desc;

    Com_Memset( entries, 0, sizeof( entries ) );

    /* binding 0: uniform buffer */
    entries[0].binding               = 0;
    entries[0].visibility            = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[0].buffer.type           = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof( wgpuUniforms2D_t );

    /* binding 1: texture */
    entries[1].binding                  = 1;
    entries[1].visibility               = WGPUShaderStage_Fragment;
    entries[1].texture.sampleType       = WGPUTextureSampleType_Float;
    entries[1].texture.viewDimension    = WGPUTextureViewDimension_2D;
    entries[1].texture.multisampled     = 0;

    /* binding 2: sampler */
    entries[2].binding          = 2;
    entries[2].visibility       = WGPUShaderStage_Fragment;
    entries[2].sampler.type     = WGPUSamplerBindingType_Filtering;

    Com_Memset( &desc, 0, sizeof( desc ) );
    desc.entryCount = 3;
    desc.entries    = entries;
    desc.label      = "BGL 2D";

    return wgpuDeviceCreateBindGroupLayout( wgpu.device, &desc );
}
#endif

/* =========================================================================
 * WR_BuildPipeline2D – create a 2D render pipeline for the given blend mode.
 * ========================================================================= */

#ifdef __EMSCRIPTEN__
static void WR_BuildPipeline2D( wgpuBlend_t blend )
{
    wgpuPipeline_t            *p = &wgpu.pipe2D[blend];
    WGPUBindGroupLayout        bgl;
    WGPUPipelineLayoutDescriptor plDesc;
    WGPURenderPipelineDescriptor rpDesc;
    WGPUVertexAttribute        attrs[3];
    WGPUVertexBufferLayout     vbLayout;
    WGPUBlendState             blendState;
    WGPUColorTargetState       colorTarget;

    bgl = BuildBindGroupLayout2D();

    Com_Memset( &plDesc, 0, sizeof( plDesc ) );
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts     = &bgl;
    plDesc.label                = "PL 2D";

    p->bgl    = bgl;
    p->layout = wgpuDeviceCreatePipelineLayout( wgpu.device, &plDesc );

    /* ------------------------------------------------------------------
     * Vertex attributes
     *   attr 0: float32x2  pos   (offset  0)
     *   attr 1: float32x2  uv    (offset  8)
     *   attr 2: float32x4  color (offset 16)
     * ------------------------------------------------------------------ */
    Com_Memset( attrs, 0, sizeof( attrs ) );

    attrs[0].shaderLocation = 0;
    attrs[0].format         = WGPUVertexFormat_Float32x2;
    attrs[0].offset         = 0;

    attrs[1].shaderLocation = 1;
    attrs[1].format         = WGPUVertexFormat_Float32x2;
    attrs[1].offset         = 8;

    attrs[2].shaderLocation = 2;
    attrs[2].format         = WGPUVertexFormat_Float32x4;
    attrs[2].offset         = 16;

    Com_Memset( &vbLayout, 0, sizeof( vbLayout ) );
    vbLayout.arrayStride    = sizeof( wgpuVert2D_t );   /* 32 bytes */
    vbLayout.stepMode       = WGPUVertexStepMode_Vertex;
    vbLayout.attributeCount = 3;
    vbLayout.attributes     = attrs;

    /* ------------------------------------------------------------------
     * Blend state
     * ------------------------------------------------------------------ */
    Com_Memset( &blendState, 0, sizeof( blendState ) );

    switch ( blend )
    {
    case WGPU_BLEND_ALPHA:
        blendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blendState.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blendState.color.operation = WGPUBlendOperation_Add;
        blendState.alpha.srcFactor = WGPUBlendFactor_One;
        blendState.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blendState.alpha.operation = WGPUBlendOperation_Add;
        break;

    case WGPU_BLEND_ADD:
        blendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blendState.color.dstFactor = WGPUBlendFactor_One;
        blendState.color.operation = WGPUBlendOperation_Add;
        blendState.alpha.srcFactor = WGPUBlendFactor_Zero;
        blendState.alpha.dstFactor = WGPUBlendFactor_One;
        blendState.alpha.operation = WGPUBlendOperation_Add;
        break;

    default: /* WGPU_BLEND_OPAQUE */
        blendState.color.srcFactor = WGPUBlendFactor_One;
        blendState.color.dstFactor = WGPUBlendFactor_Zero;
        blendState.color.operation = WGPUBlendOperation_Add;
        blendState.alpha.srcFactor = WGPUBlendFactor_One;
        blendState.alpha.dstFactor = WGPUBlendFactor_Zero;
        blendState.alpha.operation = WGPUBlendOperation_Add;
        break;
    }

    Com_Memset( &colorTarget, 0, sizeof( colorTarget ) );
    colorTarget.format    = WGPUTextureFormat_BGRA8Unorm;
    colorTarget.blend     = &blendState;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    /* ------------------------------------------------------------------
     * Pipeline descriptor
     * ------------------------------------------------------------------ */
    Com_Memset( &rpDesc, 0, sizeof( rpDesc ) );
    rpDesc.label  = "RP 2D";
    rpDesc.layout = p->layout;

    rpDesc.vertex.module      = wgpu.shaderMod2D;
    rpDesc.vertex.entryPoint  = "vs_main";
    rpDesc.vertex.bufferCount = 1;
    rpDesc.vertex.buffers     = &vbLayout;

    rpDesc.primitive.topology         = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
    rpDesc.primitive.frontFace        = WGPUFrontFace_CCW;
    rpDesc.primitive.cullMode         = WGPUCullMode_None;

    rpDesc.multisample.count = 1;
    rpDesc.multisample.mask  = 0xFFFFFFFF;

    WGPUFragmentState fragState;
    Com_Memset( &fragState, 0, sizeof( fragState ) );
    fragState.module      = wgpu.shaderMod2D;
    fragState.entryPoint  = "fs_main";
    fragState.targetCount = 1;
    fragState.targets     = &colorTarget;
    rpDesc.fragment       = &fragState;

    /* No depth/stencil for 2D */
    rpDesc.depthStencil = NULL;

    p->pipeline = wgpuDeviceCreateRenderPipeline( wgpu.device, &rpDesc );
    if ( !p->pipeline )
        ri.Error( ERR_FATAL, "WR_BuildPipeline2D: pipeline creation failed (blend=%d)", blend );

    p->blend = blend;
    p->valid = qtrue;
}
#endif

/* =========================================================================
 * WR_InitPipelines – build all cached pipelines.
 * ========================================================================= */

void WR_InitPipelines( void )
{
#ifdef __EMSCRIPTEN__
    int i;
    for ( i = 0; i < WGPU_BLEND_COUNT; i++ )
        WR_BuildPipeline2D( (wgpuBlend_t)i );
#endif
}

/* =========================================================================
 * WR_GetPipeline2D – return a cached 2D pipeline.
 * ========================================================================= */

wgpuPipeline_t *WR_GetPipeline2D( wgpuBlend_t blend )
{
    if ( (unsigned)blend >= WGPU_BLEND_COUNT )
        blend = WGPU_BLEND_ALPHA;
    return &wgpu.pipe2D[blend];
}

/* =========================================================================
 * WR_MakeBindGroup2D – create a bind group for one 2D draw call.
 *
 * Caller must release the returned WGPUBindGroup after the pass ends.
 * ========================================================================= */

WGPUBindGroup WR_MakeBindGroup2D( const wgpuPipeline_t *pipe,
                                   WGPUBuffer ub,
                                   WGPUTextureView tv,
                                   WGPUSampler samp )
{
#ifdef __EMSCRIPTEN__
    WGPUBindGroupEntry    entries[3];
    WGPUBindGroupDescriptor desc;

    Com_Memset( entries, 0, sizeof( entries ) );

    entries[0].binding = 0;
    entries[0].buffer  = ub;
    entries[0].offset  = 0;
    entries[0].size    = sizeof( wgpuUniforms2D_t );

    entries[1].binding     = 1;
    entries[1].textureView = tv;

    entries[2].binding = 2;
    entries[2].sampler = samp;

    Com_Memset( &desc, 0, sizeof( desc ) );
    desc.layout     = pipe->bgl;
    desc.entryCount = 3;
    desc.entries    = entries;
    desc.label      = "BG 2D draw";

    return wgpuDeviceCreateBindGroup( wgpu.device, &desc );
#else
    (void)pipe; (void)ub; (void)tv; (void)samp;
    return NULL;
#endif
}

#endif /* USE_WEBGPU */
