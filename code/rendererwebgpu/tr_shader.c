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
 * tr_shader.c – simplified material (shader) registry for the WebGPU
 * renderer.
 *
 * The full Q3 shader system (script parsing, multiple stages, deforms, …)
 * is not replicated here.  Instead each material maps to at most one image
 * and one blend mode, which is sufficient for rendering the HUD / menus and
 * basic world geometry.
 *
 * Handles returned to the engine are 1-based material indices (0 = white).
 */

#ifdef USE_WEBGPU

#include "tr_local.h"

/* =========================================================================
 * WR_InitShaders
 * ========================================================================= */

void WR_InitShaders( void )
{
    Com_Memset( wgpu.mats, 0, sizeof( wgpu.mats ) );
    wgpu.numMats = 0;

    /* Slot 0 = default "white" material */
    Q_strncpyz( wgpu.mats[0].name, "$default", sizeof( wgpu.mats[0].name ) );
    wgpu.mats[0].imageIndex = wgpu.whiteImageIdx;
    wgpu.mats[0].flags      = 0;
    wgpu.numMats            = 1;
}

/* =========================================================================
 * WR_FindShader – linear search by name, returns index or -1
 * ========================================================================= */

int WR_FindShader( const char *name )
{
    int i;
    if ( !name || !name[0] )
        return -1;
    for ( i = 0; i < wgpu.numMats; i++ )
    {
        if ( Q_stricmp( wgpu.mats[i].name, name ) == 0 )
            return i;
    }
    return -1;
}

/* =========================================================================
 * WR_RegisterShader – find or create a material entry for 'name'.
 *
 * 'name' may be:
 *   – a shader script name  (look up first stage image)
 *   – a bare image path     (load the image directly)
 *
 * Returns a 1-based handle suitable for returning from RE_RegisterShader.
 * On failure returns 0 (white material).
 * ========================================================================= */

int WR_RegisterShader( const char *name, int flags )
{
    int  idx;
    int  imageIdx;

    if ( !name || !name[0] )
        return 0;

    /* Check cache */
    idx = WR_FindShader( name );
    if ( idx >= 0 )
        return idx;

    /* Table full? */
    if ( wgpu.numMats >= MAX_WEBGPU_MATS )
    {
        ri.Printf( PRINT_WARNING,
                   "WR_RegisterShader: material table full, ignoring '%s'\n",
                   name );
        return 0;
    }

    /* Try to load the image.  WR_LoadImage falls back to the white image
       if the file cannot be found. */
    imageIdx = WR_LoadImage( name );

    idx = wgpu.numMats++;
    Q_strncpyz( wgpu.mats[idx].name, name, sizeof( wgpu.mats[idx].name ) );
    wgpu.mats[idx].imageIndex = imageIdx;
    wgpu.mats[idx].flags      = flags;
    wgpu.mats[idx].sort       = 3.0f;  /* SS_OPAQUE */

    return idx;
}

/* =========================================================================
 * WR_RegisterShaderFromData – create a material backed by raw RGBA pixels.
 * ========================================================================= */

int WR_RegisterShaderFromData( const char *name, const byte *rgba,
                                int width, int height,
                                qboolean mipmap, qboolean clampToEdge )
{
    int imageIdx;
    int idx;

    if ( !name || !name[0] )
        return 0;

    idx = WR_FindShader( name );
    if ( idx >= 0 )
        return idx;

    if ( wgpu.numMats >= MAX_WEBGPU_MATS )
        return 0;

    imageIdx = WR_CreateImageFromData( name, rgba, width, height,
                                        mipmap, clampToEdge );

    idx = wgpu.numMats++;
    Q_strncpyz( wgpu.mats[idx].name, name, sizeof( wgpu.mats[idx].name ) );
    wgpu.mats[idx].imageIndex = imageIdx;
    wgpu.mats[idx].flags      = WGPU_MAT_BLEND;
    wgpu.mats[idx].sort       = 9.0f;  /* SS_BLEND0 */

    return idx;
}

#endif /* USE_WEBGPU */
