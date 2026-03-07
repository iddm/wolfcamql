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
 * tr_world.c – BSP world loading and rendering for the WebGPU renderer.
 *
 * Supports:
 *   MST_PLANAR        – polygon mesh faces
 *   MST_TRIANGLE_SOUP – triangle soup
 *   MST_PATCH         – biquadratic Bezier patches (tessellated on load)
 *   MST_FLARE         – ignored
 *
 * All geometry is uploaded as a single pair of static VBO/IBO on map load.
 * At render time, WR_AddWorldToFrame() queues one wgpuDraw3D_t per surface.
 */

#ifdef USE_WEBGPU

#include "tr_local.h"

/* =========================================================================
 * Biquadratic Bezier patch tessellation
 * ========================================================================= */

/* Evaluate biquadratic (quadratic) Bernstein basis functions at t. */
static void BezierBasis( float t, float b[3] )
{
    float inv = 1.0f - t;
    b[0] = inv * inv;
    b[1] = 2.0f * t * inv;
    b[2] = t * t;
}

/*
 * EvalPatch – evaluate the surface at (s,t) given a 3×3 grid of control points.
 * cp[row][col] where row is the "v" direction, col the "u" direction.
 */
static wgpuVert3D_t EvalPatch( const drawVert_t *cp[3][3], float s, float t )
{
    float        bs[3], bt[3];
    wgpuVert3D_t r;
    int          i, j;
    float        w;

    BezierBasis( s, bs );
    BezierBasis( t, bt );

    Com_Memset( &r, 0, sizeof( r ) );

    for ( i = 0; i < 3; i++ )      /* row (t direction) */
    {
        for ( j = 0; j < 3; j++ )  /* col (s direction) */
        {
            w    = bt[i] * bs[j];
            r.x += cp[i][j]->xyz[0] * w;
            r.y += cp[i][j]->xyz[1] * w;
            r.z += cp[i][j]->xyz[2] * w;
            r.u += cp[i][j]->st[0]  * w;
            r.v += cp[i][j]->st[1]  * w;
        }
    }
    return r;
}

/*
 * TessellatePatch – tessellate one 3×3 block of control points into the
 * output vertex / index arrays.
 *
 * Returns qtrue on success, qfalse if there is no room.
 */
static qboolean TessellatePatch( const drawVert_t  *srcVerts,
                                  int                patchWidth,
                                  int                blockCol,    /* 0-based block column */
                                  int                blockRow,    /* 0-based block row    */
                                  wgpuVert3D_t      *outVerts,
                                  int               *numOutVerts,
                                  int               *outIndexes,
                                  int               *numOutIndexes,
                                  int                maxVerts,
                                  int                maxIndexes )
{
    const drawVert_t *cp[3][3];
    int               level = WGPU_TESS_LEVEL;
    int               gridW = level + 1;   /* verts along each axis */
    int               baseV = *numOutVerts;
    int               si, ti, vi;
    float             s, t;

    /* Fill control point pointers (row-major in BSP: verts[row*patchWidth+col]) */
    {
        int r, c;
        for ( r = 0; r < 3; r++ )
        {
            for ( c = 0; c < 3; c++ )
            {
                cp[r][c] = srcVerts + ( blockRow * 2 + r ) * patchWidth
                                    + ( blockCol * 2 + c );
            }
        }
    }

    /* Check bounds */
    if ( *numOutVerts + gridW * gridW > maxVerts )
        return qfalse;
    if ( *numOutIndexes + level * level * 6 > maxIndexes )
        return qfalse;

    /* Evaluate grid */
    vi = 0;
    for ( ti = 0; ti <= level; ti++ )
    {
        t = (float)ti / (float)level;
        for ( si = 0; si <= level; si++ )
        {
            s = (float)si / (float)level;
            outVerts[baseV + vi] = EvalPatch( cp, s, t );
            vi++;
        }
    }
    *numOutVerts += gridW * gridW;

    /* Generate indices (two triangles per quad) */
    for ( ti = 0; ti < level; ti++ )
    {
        for ( si = 0; si < level; si++ )
        {
            int i00 = baseV + ti * gridW + si;
            int i10 = baseV + ti * gridW + si + 1;
            int i01 = baseV + ( ti + 1 ) * gridW + si;
            int i11 = baseV + ( ti + 1 ) * gridW + si + 1;

            outIndexes[*numOutIndexes + 0] = i00;
            outIndexes[*numOutIndexes + 1] = i10;
            outIndexes[*numOutIndexes + 2] = i11;
            outIndexes[*numOutIndexes + 3] = i00;
            outIndexes[*numOutIndexes + 4] = i11;
            outIndexes[*numOutIndexes + 5] = i01;
            *numOutIndexes += 6;
        }
    }

    return qtrue;
}

/* =========================================================================
 * WR_ClearWorld – release world GPU resources.
 * ========================================================================= */

void WR_ClearWorld( void )
{
#ifdef __EMSCRIPTEN__
    if ( wgpu.worldVB )
    {
        wgpuBufferDestroy( wgpu.worldVB );
        wgpuBufferRelease( wgpu.worldVB );
        wgpu.worldVB = NULL;
    }
    if ( wgpu.worldIB )
    {
        wgpuBufferDestroy( wgpu.worldIB );
        wgpuBufferRelease( wgpu.worldIB );
        wgpu.worldIB = NULL;
    }
#endif

    if ( wgpu.worldSurfs )
    {
        ri.Free( wgpu.worldSurfs );
        wgpu.worldSurfs = NULL;
    }
    wgpu.numWorldSurfs = 0;
    wgpu.worldLoaded   = qfalse;
}

/* =========================================================================
 * WR_LoadWorld – load and upload BSP world geometry.
 * ========================================================================= */

void WR_LoadWorld( const char *name )
{
    void        *fileData;
    int          fileLen;
    dheader_t   *header;
    drawVert_t  *bspVerts;
    int         *bspIndexes;
    dsurface_t  *bspSurfs;
    dshader_t   *bspShaders;
    int          numBspVerts, numBspIndexes, numBspSurfs, numBspShaders;
    wgpuVert3D_t *outVerts  = NULL;
    int          *outIndexes = NULL;
    wgpuWorldSurf_t *outSurfs = NULL;
    int           numOutVerts = 0, numOutIndexes = 0, numOutSurfs = 0;
    int           i;
    lump_t       *lumps;

    WR_ClearWorld();

    /* ---- Load file -------------------------------------------------- */
    fileLen = ri.FS_ReadFile( name, &fileData );
    if ( fileLen <= 0 || !fileData )
    {
        ri.Printf( PRINT_WARNING, "WR_LoadWorld: cannot read '%s'\n", name );
        return;
    }

    header = (dheader_t *)fileData;
    if ( LittleLong( header->ident )   != BSP_IDENT ||
         LittleLong( header->version ) != BSP_VERSION )
    {
        ri.Printf( PRINT_WARNING, "WR_LoadWorld: '%s' is not a valid BSP\n", name );
        ri.FS_FreeFile( fileData );
        return;
    }

    lumps = header->lumps;

    /* ---- Get lump pointers ------------------------------------------ */
    bspVerts   = (drawVert_t *)( (byte *)fileData + lumps[LUMP_DRAWVERTS].fileofs );
    numBspVerts = lumps[LUMP_DRAWVERTS].filelen / sizeof( drawVert_t );

    bspIndexes   = (int *)( (byte *)fileData + lumps[LUMP_DRAWINDEXES].fileofs );
    numBspIndexes = lumps[LUMP_DRAWINDEXES].filelen / sizeof( int );

    bspSurfs   = (dsurface_t *)( (byte *)fileData + lumps[LUMP_SURFACES].fileofs );
    numBspSurfs = lumps[LUMP_SURFACES].filelen / sizeof( dsurface_t );

    bspShaders   = (dshader_t *)( (byte *)fileData + lumps[LUMP_SHADERS].fileofs );
    numBspShaders = lumps[LUMP_SHADERS].filelen / sizeof( dshader_t );

    ri.Printf( PRINT_ALL, "WR_LoadWorld: '%s' – %d surfs, %d verts, %d indexes\n",
               name, numBspSurfs, numBspVerts, numBspIndexes );

    /* ---- Allocate CPU-side staging buffers -------------------------- */
    outVerts   = (wgpuVert3D_t *)ri.Malloc( WGPU_MAX_WORLD_VERTS   * sizeof( wgpuVert3D_t ) );
    outIndexes = (int *)          ri.Malloc( WGPU_MAX_WORLD_INDEXES * sizeof( int ) );
    outSurfs   = (wgpuWorldSurf_t *)ri.Malloc( WGPU_MAX_WORLD_SURFS * sizeof( wgpuWorldSurf_t ) );

    if ( !outVerts || !outIndexes || !outSurfs )
    {
        ri.Printf( PRINT_WARNING, "WR_LoadWorld: out of memory\n" );
        goto done;
    }

    /* ---- Process each BSP surface ----------------------------------- */
    for ( i = 0; i < numBspSurfs && numOutSurfs < WGPU_MAX_WORLD_SURFS; i++ )
    {
        const dsurface_t *surf = &bspSurfs[i];
        int               matIdx;
        int               firstIdx, j;
        const char       *shaderName;

        /* Resolve shader / material */
        if ( surf->shaderNum >= 0 && surf->shaderNum < numBspShaders )
            shaderName = bspShaders[surf->shaderNum].shader;
        else
            shaderName = "*white";
        matIdx = WR_RegisterShader( shaderName, WGPU_MAT_BLEND );

        switch ( surf->surfaceType )
        {
        case MST_PLANAR:
        case MST_TRIANGLE_SOUP:
            /* ---- Copy vertices ------------------------------------ */
            if ( numOutVerts + surf->numVerts > WGPU_MAX_WORLD_VERTS )
                goto done;
            if ( numOutIndexes + surf->numIndexes > WGPU_MAX_WORLD_INDEXES )
                goto done;

            firstIdx = numOutIndexes;

            for ( j = 0; j < surf->numVerts; j++ )
            {
                const drawVert_t *dv = &bspVerts[surf->firstVert + j];
                wgpuVert3D_t     *ov = &outVerts[numOutVerts + j];
                ov->x = dv->xyz[0];
                ov->y = dv->xyz[1];
                ov->z = dv->xyz[2];
                ov->u = dv->st[0];
                ov->v = dv->st[1];
            }

            for ( j = 0; j < surf->numIndexes; j++ )
            {
                outIndexes[numOutIndexes + j] =
                    LittleLong( bspIndexes[surf->firstIndex + j] ) + numOutVerts;
            }

            numOutVerts   += surf->numVerts;
            numOutIndexes += surf->numIndexes;

            outSurfs[numOutSurfs].matIndex  = matIdx;
            outSurfs[numOutSurfs].firstIndex = firstIdx * (int)sizeof( int );
            outSurfs[numOutSurfs].numIndexes = surf->numIndexes;
            numOutSurfs++;
            break;

        case MST_PATCH:
        {
            /* ---- Biquadratic Bezier patch ------------------------- */
            int pw = surf->patchWidth;
            int ph = surf->patchHeight;
            int numBlocksU = ( pw - 1 ) / 2;
            int numBlocksV = ( ph - 1 ) / 2;
            int bu, bv;

            firstIdx = numOutIndexes;
            int firstVert = numOutVerts;

            for ( bv = 0; bv < numBlocksV; bv++ )
            {
                for ( bu = 0; bu < numBlocksU; bu++ )
                {
                    if ( !TessellatePatch(
                             &bspVerts[surf->firstVert],
                             pw, bu, bv,
                             outVerts,   &numOutVerts,
                             outIndexes, &numOutIndexes,
                             WGPU_MAX_WORLD_VERTS,
                             WGPU_MAX_WORLD_INDEXES ) )
                    {
                        ri.Printf( PRINT_WARNING,
                                   "WR_LoadWorld: patch buffer full\n" );
                        goto done;
                    }
                }
            }

            if ( numOutIndexes > firstIdx )
            {
                outSurfs[numOutSurfs].matIndex   = matIdx;
                outSurfs[numOutSurfs].firstIndex = firstIdx * (int)sizeof( int );
                outSurfs[numOutSurfs].numIndexes = numOutIndexes - firstIdx;
                numOutSurfs++;
            }
            (void)firstVert;
            break;
        }

        case MST_FLARE:
        default:
            /* Skip flares and unknown surface types */
            break;
        }
    }

    ri.Printf( PRINT_ALL, "WR_LoadWorld: tessellated → %d verts, %d indexes, %d surfs\n",
               numOutVerts, numOutIndexes, numOutSurfs );

    /* ---- Upload to GPU ---------------------------------------------- */
#ifdef __EMSCRIPTEN__
    {
        WGPUBufferDescriptor bd;

        Com_Memset( &bd, 0, sizeof( bd ) );
        bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        bd.size  = (uint64_t)numOutVerts * sizeof( wgpuVert3D_t );
        bd.label = "World VB";
        wgpu.worldVB = wgpuDeviceCreateBuffer( wgpu.device, &bd );
        wgpuQueueWriteBuffer( wgpu.queue, wgpu.worldVB, 0,
                               outVerts,
                               (size_t)numOutVerts * sizeof( wgpuVert3D_t ) );

        Com_Memset( &bd, 0, sizeof( bd ) );
        bd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
        bd.size  = (uint64_t)numOutIndexes * sizeof( int );
        bd.label = "World IB";
        wgpu.worldIB = wgpuDeviceCreateBuffer( wgpu.device, &bd );
        wgpuQueueWriteBuffer( wgpu.queue, wgpu.worldIB, 0,
                               outIndexes,
                               (size_t)numOutIndexes * sizeof( int ) );
    }
#endif

    /* ---- Store surface array --------------------------------------- */
    wgpu.worldSurfs    = outSurfs;
    wgpu.numWorldSurfs = numOutSurfs;
    wgpu.worldLoaded   = qtrue;
    outSurfs           = NULL;   /* ownership transferred */

done:
    if ( outVerts   ) ri.Free( outVerts );
    if ( outIndexes ) ri.Free( outIndexes );
    if ( outSurfs   ) ri.Free( outSurfs );
    ri.FS_FreeFile( fileData );
}

/* =========================================================================
 * WR_AddWorldToFrame
 *
 * Queue draw calls for all visible world surfaces.  All surfaces share the
 * same MVP (the world has no per-surface model transform), so we allocate
 * a single uniform slot and reference it from every surface draw.
 * ========================================================================= */

void WR_AddWorldToFrame( void )
{
    int worldSlot;
    int i;

    if ( !wgpu.worldLoaded || !wgpu.worldVB || !wgpu.worldIB )
        return;

    /* One uniform slot for the entire world */
    {
        float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        worldSlot = WR_Alloc3DUniformSlot( wgpu.viewProj, color );
        if ( worldSlot < 0 )
            return;
    }

    for ( i = 0; i < wgpu.numWorldSurfs; i++ )
    {
        const wgpuWorldSurf_t *s = &wgpu.worldSurfs[i];
        wgpuBlend_t            blend;

        if ( s->numIndexes <= 0 )
            continue;

        /* Simple blend decision: if the material has an alpha image use alpha blend */
        if ( s->matIndex >= 0 && s->matIndex < wgpu.numMats &&
             ( wgpu.mats[s->matIndex].flags & ( WGPU_MAT_BLEND | WGPU_MAT_ADDITIVE ) ) )
            blend = ( wgpu.mats[s->matIndex].flags & WGPU_MAT_ADDITIVE )
                    ? WGPU_BLEND_ADD : WGPU_BLEND_ALPHA;
        else
            blend = WGPU_BLEND_OPAQUE;

        WR_AddDraw3D( s->matIndex, worldSlot,
                      wgpu.worldVB, 0,
                      wgpu.worldIB, (uint64_t)s->firstIndex,
                      s->numIndexes, blend );
    }
}

#endif /* USE_WEBGPU */
