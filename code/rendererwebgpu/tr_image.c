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
 * tr_image.c – WebGPU texture management.
 *
 * Handles:
 *   – Creating WGPUTexture / WGPUTextureView / WGPUSampler from raw RGBA data
 *   – Loading image files (TGA/JPG/PNG) via the renderercommon format loaders
 *   – The global image registry (name → wgpuImage_t index)
 */

#ifdef USE_WEBGPU

#include "tr_local.h"

/*
 * Image loaders from renderercommon – declared here directly so we don't
 * need to include tr_common.h again (it's already pulled in via tr_local.h).
 */
void R_LoadTGA ( const char *name, byte **pic, int *width, int *height );
void R_LoadJPG ( const char *name, byte **pic, int *width, int *height );
void R_LoadPNG ( const char *name, byte **pic, int *width, int *height );
void R_LoadBMP ( const char *name, byte **pic, int *width, int *height );

/* =========================================================================
 * WR_InitImages – reset image table, create the white fallback image.
 * ========================================================================= */

void WR_InitImages( void )
{
    static const byte white4[4] = { 255, 255, 255, 255 };

    Com_Memset( wgpu.images, 0, sizeof( wgpu.images ) );
    wgpu.numImages = 0;

    /* Slot 0 = 1×1 solid white (used when a material has no image) */
    wgpu.whiteImageIdx = WR_CreateImageFromData( "$white", white4,
                                                  1, 1, qfalse, qtrue );
}

/* =========================================================================
 * WR_FindImage – look up by name, return index or -1
 * ========================================================================= */

int WR_FindImage( const char *name )
{
    int i;
    for ( i = 0; i < wgpu.numImages; i++ )
    {
        if ( Q_stricmp( wgpu.images[i].name, name ) == 0 )
            return i;
    }
    return -1;
}

/* =========================================================================
 * WR_CreateImageFromData – upload raw RGBA bytes to a new WGPUTexture.
 *
 * Returns the image index in wgpu.images[].
 * On failure (table full, device not ready), returns wgpu.whiteImageIdx.
 * ========================================================================= */

int WR_CreateImageFromData( const char *name, const byte *rgba,
                             int width, int height,
                             qboolean mipmap, qboolean clampToEdge )
{
#ifdef __EMSCRIPTEN__
    wgpuImage_t            *img;
    int                     idx;
    WGPUTextureDescriptor   texDesc;
    WGPUTextureViewDescriptor viewDesc;
    WGPUSamplerDescriptor   sampDesc;
    WGPUImageCopyTexture    dst;
    WGPUTextureDataLayout   layout;
    WGPUExtent3D            extent;

    /* Reuse existing slot with same name */
    idx = WR_FindImage( name );
    if ( idx >= 0 )
        return idx;

    if ( wgpu.numImages >= MAX_WEBGPU_IMAGES )
    {
        ri.Printf( PRINT_WARNING,
                   "WR_CreateImageFromData: image table full, ignoring '%s'\n",
                   name );
        return wgpu.whiteImageIdx;
    }

    if ( !wgpu.device )
        return 0;   /* called before device init – return slot 0 */

    idx = wgpu.numImages++;
    img = &wgpu.images[idx];

    Q_strncpyz( img->name, name, sizeof( img->name ) );
    img->width  = width;
    img->height = height;

    /* ------------------------------------------------------------------
     * Create texture
     * ------------------------------------------------------------------ */
    Com_Memset( &texDesc, 0, sizeof( texDesc ) );
    texDesc.usage               = WGPUTextureUsage_TextureBinding
                                | WGPUTextureUsage_CopyDst;
    texDesc.dimension           = WGPUTextureDimension_2D;
    texDesc.size.width          = (uint32_t)width;
    texDesc.size.height         = (uint32_t)height;
    texDesc.size.depthOrArrayLayers = 1;
    texDesc.format              = WGPUTextureFormat_RGBA8Unorm;
    texDesc.mipLevelCount       = 1;
    texDesc.sampleCount         = 1;

    img->texture = wgpuDeviceCreateTexture( wgpu.device, &texDesc );
    if ( !img->texture )
    {
        ri.Printf( PRINT_WARNING,
                   "WR_CreateImageFromData: wgpuDeviceCreateTexture failed for '%s'\n",
                   name );
        wgpu.numImages--;
        return wgpu.whiteImageIdx;
    }

    /* ------------------------------------------------------------------
     * Upload pixel data
     * ------------------------------------------------------------------ */
    Com_Memset( &dst, 0, sizeof( dst ) );
    dst.texture   = img->texture;
    dst.mipLevel  = 0;
    dst.origin.x  = 0;
    dst.origin.y  = 0;
    dst.origin.z  = 0;
    dst.aspect    = WGPUTextureAspect_All;

    Com_Memset( &layout, 0, sizeof( layout ) );
    layout.offset       = 0;
    layout.bytesPerRow  = (uint32_t)( width * 4 );
    layout.rowsPerImage = (uint32_t)height;

    extent.width              = (uint32_t)width;
    extent.height             = (uint32_t)height;
    extent.depthOrArrayLayers = 1;

    wgpuQueueWriteTexture( wgpu.queue, &dst, rgba,
                            (size_t)( width * height * 4 ),
                            &layout, &extent );

    /* ------------------------------------------------------------------
     * Texture view
     * ------------------------------------------------------------------ */
    Com_Memset( &viewDesc, 0, sizeof( viewDesc ) );
    viewDesc.format          = WGPUTextureFormat_RGBA8Unorm;
    viewDesc.dimension       = WGPUTextureViewDimension_2D;
    viewDesc.baseMipLevel    = 0;
    viewDesc.mipLevelCount   = 1;
    viewDesc.baseArrayLayer  = 0;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect          = WGPUTextureAspect_All;

    img->view = wgpuTextureCreateView( img->texture, &viewDesc );

    /* ------------------------------------------------------------------
     * Sampler
     * ------------------------------------------------------------------ */
    Com_Memset( &sampDesc, 0, sizeof( sampDesc ) );
    {
        WGPUAddressMode wrap = clampToEdge
            ? WGPUAddressMode_ClampToEdge
            : WGPUAddressMode_Repeat;
        sampDesc.addressModeU  = wrap;
        sampDesc.addressModeV  = wrap;
        sampDesc.addressModeW  = WGPUAddressMode_ClampToEdge;
    }
    sampDesc.magFilter    = WGPUFilterMode_Linear;
    sampDesc.minFilter    = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sampDesc.maxAnisotropy = 1;

    img->sampler = wgpuDeviceCreateSampler( wgpu.device, &sampDesc );

    return idx;

#else   /* non-Emscripten stub */
    (void)name; (void)rgba; (void)width; (void)height;
    (void)mipmap; (void)clampToEdge;
    return 0;
#endif
}

/* =========================================================================
 * WR_LoadImage – load an image file and upload to WebGPU.
 *
 * Tries TGA, then JPG, then PNG.  Falls back to the white image on failure.
 * ========================================================================= */

int WR_LoadImage( const char *name )
{
    byte  *pic    = NULL;
    int    width  = 0;
    int    height = 0;
    int    idx;
    char   altName[MAX_QPATH];
    const char *ext;

    /* Already loaded? */
    idx = WR_FindImage( name );
    if ( idx >= 0 )
        return idx;

    ext = strrchr( name, '.' );

    if ( ext && Q_stricmp( ext, ".tga" ) == 0 )
        R_LoadTGA( name, &pic, &width, &height );
    else if ( ext && Q_stricmp( ext, ".jpg" ) == 0 )
        R_LoadJPG( name, &pic, &width, &height );
    else if ( ext && Q_stricmp( ext, ".png" ) == 0 )
        R_LoadPNG( name, &pic, &width, &height );
    else
    {
        /* Try TGA first, then JPG */
        COM_StripExtension( name, altName, sizeof( altName ) );
        Q_strcat( altName, sizeof( altName ), ".tga" );
        R_LoadTGA( altName, &pic, &width, &height );
        if ( !pic )
        {
            COM_StripExtension( name, altName, sizeof( altName ) );
            Q_strcat( altName, sizeof( altName ), ".jpg" );
            R_LoadJPG( altName, &pic, &width, &height );
        }
        if ( !pic )
        {
            COM_StripExtension( name, altName, sizeof( altName ) );
            Q_strcat( altName, sizeof( altName ), ".png" );
            R_LoadPNG( altName, &pic, &width, &height );
        }
    }

    if ( !pic || width <= 0 || height <= 0 )
    {
        if ( pic )
            ri.Hunk_FreeTempMemory( pic );
        return wgpu.whiteImageIdx;
    }

    idx = WR_CreateImageFromData( name, pic, width, height, qtrue, qfalse );
    ri.Hunk_FreeTempMemory( pic );
    return idx;
}

/* =========================================================================
 * WR_FreeImages – release all WebGPU texture resources.
 * ========================================================================= */

void WR_FreeImages( void )
{
#ifdef __EMSCRIPTEN__
    int i;
    for ( i = 0; i < wgpu.numImages; i++ )
    {
        wgpuImage_t *img = &wgpu.images[i];
        if ( img->sampler )
        {
            wgpuSamplerRelease( img->sampler );
            img->sampler = NULL;
        }
        if ( img->view )
        {
            wgpuTextureViewRelease( img->view );
            img->view = NULL;
        }
        if ( img->texture )
        {
            wgpuTextureDestroy( img->texture );
            wgpuTextureRelease( img->texture );
            img->texture = NULL;
        }
    }
#endif
    wgpu.numImages = 0;
}

#endif /* USE_WEBGPU */
