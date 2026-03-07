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
 * tr_model.c – MD3 model loading and entity rendering for the WebGPU renderer.
 *
 * Supports:
 *   RT_MODEL entities using MD3 models
 *   RT_POLY  polygons queued via RE_AddPolyToScene
 *
 * Vertex interpolation between frames happens on the CPU before uploading
 * to the per-frame dynamic vertex buffer.
 */

#ifdef USE_WEBGPU

#include "tr_local.h"
#include <math.h>

/* =========================================================================
 * MD3 decoding helpers
 * ========================================================================= */

#define MD3_XYZ_SCALE  (1.0f / 64.0f)

/*
 * Decode a single MD3 vertex (compressed short[3] + normal short) into
 * a wgpuVert3D_t, mixing xyz only (UV comes from the md3St_t array).
 */
static void DecodeXyz( const md3XyzNormal_t *src, float *ox, float *oy, float *oz )
{
    *ox = (float)LittleShort( src->xyz[0] ) * MD3_XYZ_SCALE;
    *oy = (float)LittleShort( src->xyz[1] ) * MD3_XYZ_SCALE;
    *oz = (float)LittleShort( src->xyz[2] ) * MD3_XYZ_SCALE;
}

/* =========================================================================
 * WR_LoadModel – parse and store an MD3 model.
 *
 * Returns the index into wgpu.models[] on success, or -1 on failure.
 * Subsequent calls with the same name return the cached index.
 * ========================================================================= */

int WR_LoadModel( const char *name )
{
    void         *fileData = NULL;
    int           fileLen;
    md3Header_t  *hdr;
    wgpuModel_t  *m;
    int           modelIdx;
    int           s;
    const char   *p;

    if ( !name || !name[0] )
        return -1;

    /* Check cache */
    for ( modelIdx = 0; modelIdx < wgpu.numModels; modelIdx++ )
    {
        if ( wgpu.models[modelIdx].loaded &&
             !Q_stricmp( wgpu.models[modelIdx].name, name ) )
            return modelIdx;
    }

    if ( wgpu.numModels >= WGPU_MAX_MODELS )
    {
        ri.Printf( PRINT_WARNING, "WR_LoadModel: model limit reached\n" );
        return -1;
    }

    /* Try to load; support .md3 extension auto-append */
    fileLen = ri.FS_ReadFile( name, &fileData );
    if ( fileLen <= 0 || !fileData )
    {
        /* Try with .md3 extension */
        char alt[MAX_QPATH];
        Q_strncpyz( alt, name, sizeof( alt ) );
        COM_StripExtension( alt, alt, sizeof( alt ) );
        Q_strcat( alt, sizeof( alt ), ".md3" );
        fileLen = ri.FS_ReadFile( alt, &fileData );
        if ( fileLen <= 0 || !fileData )
        {
            ri.Printf( PRINT_DEVELOPER, "WR_LoadModel: cannot find '%s'\n", name );
            return -1;
        }
    }

    hdr = (md3Header_t *)fileData;

    if ( LittleLong( hdr->ident )   != MD3_IDENT ||
         LittleLong( hdr->version ) != MD3_VERSION )
    {
        ri.Printf( PRINT_WARNING, "WR_LoadModel: '%s' is not a valid MD3\n", name );
        ri.FS_FreeFile( fileData );
        return -1;
    }

    modelIdx = wgpu.numModels++;
    m        = &wgpu.models[modelIdx];
    Com_Memset( m, 0, sizeof( *m ) );
    Q_strncpyz( m->name, name, sizeof( m->name ) );

    m->numFrames = LittleLong( hdr->numFrames );
    m->numSurfs  = 0;

    /* ---- Iterate MD3 surfaces ---------------------------------------- */
    p = (const char *)fileData + LittleLong( hdr->ofsSurfaces );

    for ( s = 0; s < LittleLong( hdr->numSurfaces ) &&
                 m->numSurfs < WGPU_MAX_MODEL_SURFS; s++ )
    {
        const md3Surface_t *ms  = (const md3Surface_t *)p;
        wgpuModelSurf_t    *out = &m->surfs[m->numSurfs];
        const md3Shader_t  *shaders;
        const md3Triangle_t *tris;
        const md3St_t       *sts;
        const md3XyzNormal_t *xyz;
        int                  nv, nf, ni, v, f;

        nv = LittleLong( ms->numVerts );
        nf = LittleLong( ms->numFrames );
        ni = LittleLong( ms->numTriangles ) * 3;

        /* Shader (use first shader) */
        shaders = (const md3Shader_t *)( (const char *)ms + LittleLong( ms->ofsShaders ) );
        out->matIndex = WR_RegisterShader(
            ( LittleLong( ms->numShaders ) > 0 ) ? shaders[0].name : "*white",
            WGPU_MAT_BLEND );

        /* Triangles → indexes */
        tris = (const md3Triangle_t *)( (const char *)ms + LittleLong( ms->ofsTriangles ) );
        out->indexes = (int *)ri.Malloc( ni * sizeof( int ) );
        if ( !out->indexes )
            goto nextsurf;
        for ( v = 0; v < ni; v++ )
            out->indexes[v] = LittleLong( ( (const int *)tris )[v] );
        out->numIndexes = ni;

        /* Texture coords */
        sts = (const md3St_t *)( (const char *)ms + LittleLong( ms->ofsSt ) );

        /* Vertex positions: numFrames * numVerts */
        xyz = (const md3XyzNormal_t *)( (const char *)ms + LittleLong( ms->ofsXyzNormals ) );
        out->frameVerts = (wgpuVert3D_t *)ri.Malloc( nf * nv * sizeof( wgpuVert3D_t ) );
        if ( !out->frameVerts )
        {
            ri.Free( out->indexes );
            out->indexes = NULL;
            goto nextsurf;
        }

        for ( f = 0; f < nf; f++ )
        {
            for ( v = 0; v < nv; v++ )
            {
                wgpuVert3D_t     *ov  = &out->frameVerts[f * nv + v];
                const md3XyzNormal_t *src = &xyz[f * nv + v];
                DecodeXyz( src, &ov->x, &ov->y, &ov->z );
                ov->u = LittleFloat( sts[v].st[0] );
                ov->v = LittleFloat( sts[v].st[1] );
            }
        }

        out->numVerts  = nv;
        out->numFrames = nf;
        m->numSurfs++;

nextsurf:
        p += LittleLong( ms->ofsEnd );
    }

    m->loaded = qtrue;
    ri.FS_FreeFile( fileData );

    ri.Printf( PRINT_DEVELOPER, "WR_LoadModel: loaded '%s' (%d surfs, %d frames)\n",
               name, m->numSurfs, m->numFrames );
    return modelIdx;
}

/* =========================================================================
 * WR_FreeModels – release all loaded model data.
 * ========================================================================= */

void WR_FreeModels( void )
{
    int m, s;
    for ( m = 0; m < wgpu.numModels; m++ )
    {
        wgpuModel_t *model = &wgpu.models[m];
        if ( !model->loaded ) continue;
        for ( s = 0; s < model->numSurfs; s++ )
        {
            wgpuModelSurf_t *surf = &model->surfs[s];
            if ( surf->frameVerts ) { ri.Free( surf->frameVerts ); surf->frameVerts = NULL; }
            if ( surf->indexes    ) { ri.Free( surf->indexes );    surf->indexes    = NULL; }
        }
        Com_Memset( model, 0, sizeof( *model ) );
    }
    wgpu.numModels = 0;
}

/* =========================================================================
 * WR_AddEntityToFrame
 *
 * CPU-interpolate the model's vertices for the current frame pair, append
 * to the dynamic vertex / index buffers, and queue a wgpuDraw3D_t per
 * surface.
 * ========================================================================= */

void WR_AddEntityToFrame( const wgpuEntity3D_t *ent )
{
    wgpuModel_t *model;
    float        entMvp[16];
    int          s;

    if ( ent->modelIndex < 0 || ent->modelIndex >= wgpu.numModels )
        return;

    model = &wgpu.models[ent->modelIndex];
    if ( !model->loaded || model->numSurfs == 0 )
        return;

    /* Build entity MVP = viewProj * modelMatrix */
    WR_BuildModelMvp( entMvp, wgpu.viewProj, ent->origin, ent->axis );

    for ( s = 0; s < model->numSurfs; s++ )
    {
        wgpuModelSurf_t *surf = &model->surfs[s];
        int               frame0, frame1, nv, ni, uniformSlot;
        float             lerp;
        int               baseVert, baseIndex;
        wgpuVert3D_t     *dv;
        int              *di;
        int               v;
        wgpuBlend_t       blend;

        if ( surf->numVerts <= 0 || surf->numIndexes <= 0 )
            continue;

        nv = surf->numVerts;
        ni = surf->numIndexes;

        /* Clamp frames */
        frame0 = ent->frame;
        frame1 = ent->oldframe;
        if ( frame0 < 0 || frame0 >= surf->numFrames ) frame0 = 0;
        if ( frame1 < 0 || frame1 >= surf->numFrames ) frame1 = 0;
        lerp = ent->backlerp;

        /* Allocate from dynamic buffers */
        if ( wgpu.numDynVerts3D + nv > WGPU_MAX_DYN_VERTS3D )
        {
            ri.Printf( PRINT_DEVELOPER, "WR_AddEntityToFrame: dynamic vertex buffer full\n" );
            return;
        }
        if ( wgpu.numDynIndexes3D + ni > WGPU_MAX_DYN_INDEXES3D )
        {
            ri.Printf( PRINT_DEVELOPER, "WR_AddEntityToFrame: dynamic index buffer full\n" );
            return;
        }

        baseVert  = wgpu.numDynVerts3D;
        baseIndex = wgpu.numDynIndexes3D;

        /* Interpolate vertices */
        dv = &wgpu.dynVerts3D[baseVert];
        {
            const wgpuVert3D_t *v0 = &surf->frameVerts[frame0 * nv];
            const wgpuVert3D_t *v1 = &surf->frameVerts[frame1 * nv];
            float               f0 = 1.0f - lerp, f1 = lerp;

            for ( v = 0; v < nv; v++ )
            {
                dv[v].x = v0[v].x * f0 + v1[v].x * f1;
                dv[v].y = v0[v].y * f0 + v1[v].y * f1;
                dv[v].z = v0[v].z * f0 + v1[v].z * f1;
                dv[v].u = v0[v].u;
                dv[v].v = v0[v].v;
            }
        }

        /* Remap indexes (relative to baseVert in the dynamic buffer) */
        di = &wgpu.dynIndexes3D[baseIndex];
        for ( v = 0; v < ni; v++ )
            di[v] = surf->indexes[v] + baseVert;

        wgpu.numDynVerts3D   += nv;
        wgpu.numDynIndexes3D += ni;

        /* Allocate uniform slot */
        uniformSlot = WR_Alloc3DUniformSlot( entMvp, ent->color );
        if ( uniformSlot < 0 )
            return;

        /* Determine blend mode */
        blend = ( surf->matIndex >= 0 && surf->matIndex < wgpu.numMats &&
                  ( wgpu.mats[surf->matIndex].flags & WGPU_MAT_ADDITIVE ) )
                ? WGPU_BLEND_ADD
                : ( surf->matIndex >= 0 && surf->matIndex < wgpu.numMats &&
                    ( wgpu.mats[surf->matIndex].flags & WGPU_MAT_BLEND ) )
                ? WGPU_BLEND_ALPHA
                : WGPU_BLEND_OPAQUE;

        WR_AddDraw3D( surf->matIndex, uniformSlot,
                      wgpu.dynVB3D,
                      (uint64_t)( baseIndex * sizeof( int ) ),  /* vbOffset unused for indexed */
                      wgpu.dynIB3D,
                      (uint64_t)( baseIndex * sizeof( int ) ),
                      ni, blend );
    }
}

/* =========================================================================
 * WR_AddPolysToFrame
 *
 * Convert queued polys to triangles and add them to the dynamic draw list.
 * ========================================================================= */

void WR_AddPolysToFrame( void )
{
    int p;

    for ( p = 0; p < wgpu.numPolys3D; p++ )
    {
        const wgpuPoly3D_t *poly  = &wgpu.polys3D[p];
        int                 nv    = poly->numVerts;
        int                 ni    = ( nv - 2 ) * 3;   /* fan triangulation */
        int                 baseV, baseI, uniformSlot;
        wgpuVert3D_t       *dv;
        int                *di;
        int                 v;
        const float         color[4] = { 1, 1, 1, 1 };

        if ( nv < 3 || ni <= 0 )
            continue;
        if ( wgpu.numDynVerts3D   + nv > WGPU_MAX_DYN_VERTS3D   ) break;
        if ( wgpu.numDynIndexes3D + ni > WGPU_MAX_DYN_INDEXES3D ) break;

        baseV = wgpu.numDynVerts3D;
        baseI = wgpu.numDynIndexes3D;

        dv = &wgpu.dynVerts3D[baseV];
        for ( v = 0; v < nv; v++ )
        {
            dv[v].x = poly->verts[v].xyz[0];
            dv[v].y = poly->verts[v].xyz[1];
            dv[v].z = poly->verts[v].xyz[2];
            dv[v].u = poly->verts[v].st[0];
            dv[v].v = poly->verts[v].st[1];
        }

        di = &wgpu.dynIndexes3D[baseI];
        for ( v = 0; v < nv - 2; v++ )
        {
            di[v * 3 + 0] = baseV;
            di[v * 3 + 1] = baseV + v + 1;
            di[v * 3 + 2] = baseV + v + 2;
        }

        wgpu.numDynVerts3D   += nv;
        wgpu.numDynIndexes3D += ni;

        uniformSlot = WR_Alloc3DUniformSlot( wgpu.viewProj, color );
        if ( uniformSlot < 0 ) break;

        WR_AddDraw3D( poly->matIndex, uniformSlot,
                      wgpu.dynVB3D, 0,
                      wgpu.dynIB3D, (uint64_t)( baseI * sizeof( int ) ),
                      ni, WGPU_BLEND_ALPHA );
    }
}

#endif /* USE_WEBGPU */
