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
 * Stub qgl.h for the WebGPU renderer.
 *
 * Provides GL type compatibility so renderercommon source files
 * (tr_image_tga.c, tr_image_jpg.c, tr_noise.c, tr_subs.c, tr_font.c, …)
 * compile without pulling in the real OpenGL / SDL_opengl headers.
 *
 * None of the GL function pointers are initialised; those files must not
 * call any qgl* function at runtime when compiled for the WebGPU path.
 */

#ifndef __QGL_H__
#define __QGL_H__

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * GL scalar types
 * ---------------------------------------------------------------------- */
typedef unsigned int    GLenum;
typedef unsigned char   GLboolean;
typedef unsigned int    GLbitfield;
typedef void            GLvoid;
typedef signed char     GLbyte;
typedef short           GLshort;
typedef int             GLint;
typedef unsigned char   GLubyte;
typedef unsigned short  GLushort;
typedef unsigned int    GLuint;
typedef int             GLsizei;
typedef float           GLfloat;
typedef float           GLclampf;
typedef double          GLdouble;
typedef double          GLclampd;
typedef char            GLchar;
typedef intptr_t        GLintptr;
typedef ptrdiff_t       GLsizeiptr;
typedef GLuint          GLhandleARB;    /* ARB object handle */

#ifndef APIENTRYP
#  define APIENTRYP *
#endif

/* -------------------------------------------------------------------------
 * Essential GL constants used by tr_common.h / image struct fields
 * ---------------------------------------------------------------------- */
#define GL_FALSE                        0
#define GL_TRUE                         1

#define GL_BYTE                         0x1400
#define GL_UNSIGNED_BYTE                0x1401
#define GL_SHORT                        0x1402
#define GL_UNSIGNED_SHORT               0x1403
#define GL_INT                          0x1404
#define GL_UNSIGNED_INT                 0x1405
#define GL_FLOAT                        0x1406
#define GL_DOUBLE                       0x140A

#define GL_NONE                         0
#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_TRIANGLE_FAN                 0x0006

#define GL_ZERO                         0
#define GL_ONE                          1
#define GL_SRC_ALPHA                    0x0302
#define GL_ONE_MINUS_SRC_ALPHA          0x0303
#define GL_DST_COLOR                    0x0306
#define GL_ONE_MINUS_DST_COLOR          0x0307
#define GL_SRC_ALPHA_SATURATE           0x0308
#define GL_SRC_COLOR                    0x0300
#define GL_ONE_MINUS_SRC_COLOR          0x0301
#define GL_DST_ALPHA                    0x0304
#define GL_ONE_MINUS_DST_ALPHA          0x0305

#define GL_NEVER                        0x0200
#define GL_LESS                         0x0201
#define GL_EQUAL                        0x0202
#define GL_LEQUAL                       0x0203
#define GL_GREATER                      0x0204
#define GL_NOTEQUAL                     0x0205
#define GL_GEQUAL                       0x0206
#define GL_ALWAYS                       0x0207

#define GL_CULL_FACE                    0x0B44
#define GL_FRONT                        0x0404
#define GL_BACK                         0x0405
#define GL_FRONT_AND_BACK               0x0408

#define GL_DEPTH_TEST                   0x0B71
#define GL_BLEND                        0x0BE2
#define GL_STENCIL_TEST                 0x0B90
#define GL_SCISSOR_TEST                 0x0C11

#define GL_TEXTURE_2D                   0x0DE1
#define GL_TEXTURE_CUBE_MAP             0x8513
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_LINEAR                       0x2601
#define GL_NEAREST                      0x2600
#define GL_LINEAR_MIPMAP_LINEAR         0x2703
#define GL_LINEAR_MIPMAP_NEAREST        0x2701
#define GL_NEAREST_MIPMAP_LINEAR        0x2702
#define GL_NEAREST_MIPMAP_NEAREST       0x2700
#define GL_REPEAT                       0x2901
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_CLAMP                        0x2900

#define GL_RGB                          0x1907
#define GL_RGBA                         0x1908
#define GL_BGR                          0x80E0
#define GL_BGRA                         0x80E1
#define GL_LUMINANCE                    0x1909
#define GL_LUMINANCE_ALPHA              0x190A

#define GL_RGBA8                        0x8058
#define GL_RGB8                         0x8051
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3

#define GL_COLOR_BUFFER_BIT             0x00004000
#define GL_DEPTH_BUFFER_BIT             0x00000100
#define GL_STENCIL_BUFFER_BIT           0x00000400

#define GL_DEPTH_COMPONENT              0x1902
#define GL_DEPTH_COMPONENT16            0x81A5
#define GL_DEPTH_COMPONENT24            0x81A6
#define GL_DEPTH_COMPONENT32            0x81A7

#define GL_ARRAY_BUFFER                 0x8892
#define GL_ELEMENT_ARRAY_BUFFER         0x8893
#define GL_STATIC_DRAW                  0x88B4
#define GL_DYNAMIC_DRAW                 0x88E8
#define GL_STREAM_DRAW                  0x88E0

#define GL_FRAGMENT_SHADER              0x8B30
#define GL_VERTEX_SHADER                0x8B31

#define GL_FRAMEBUFFER                  0x8D40
#define GL_RENDERBUFFER                 0x8D41
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_DEPTH_ATTACHMENT             0x8D00
#define GL_STENCIL_ATTACHMENT           0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT     0x821A
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5

#define GL_TEXTURE0                     0x84C0
#define GL_MAX_TEXTURE_IMAGE_UNITS      0x8872
#define GL_MAX_TEXTURE_SIZE             0x0D33

#define GL_VERTEX_ARRAY_BINDING         0x85B5
#define GL_WRITE_ONLY                   0x88B9

/* NVX / ATI memory info constants */
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX         0x9047
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX   0x9048
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#define GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX            0x904A
#define GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX            0x904B
#define GL_VBO_FREE_MEMORY_ATI                           0x87FB
#define GL_TEXTURE_FREE_MEMORY_ATI                       0x87FC
#define GL_RENDERBUFFER_FREE_MEMORY_ATI                  0x87FD

/* -------------------------------------------------------------------------
 * GLE / QGL_*_PROCS macros – expanded to nothing for WebGPU
 * ---------------------------------------------------------------------- */
#define GLE(ret, name, ...)   /* nothing */

#define QGL_1_1_PROCS                       /* nothing */
#define QGL_1_1_FIXED_FUNCTION_PROCS        /* nothing */
#define QGL_DESKTOP_1_1_PROCS               /* nothing */
#define QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS /* nothing */
#define QGL_ES_1_1_PROCS                    /* nothing */
#define QGL_ES_1_1_FIXED_FUNCTION_PROCS     /* nothing */
#define QGL_1_3_PROCS                       /* nothing */
#define QGL_1_5_PROCS                       /* nothing */
#define QGL_2_0_PROCS                       /* nothing */
#define QGL_3_0_PROCS                       /* nothing */
#define QGL_ARB_occlusion_query_PROCS       /* nothing */
#define QGL_ARB_framebuffer_object_PROCS    /* nothing */
#define QGL_ARB_vertex_array_object_PROCS   /* nothing */
#define QGL_EXT_direct_state_access_PROCS   /* nothing */

/* -------------------------------------------------------------------------
 * Function pointer stubs – declared as void* so code that takes their
 * address (e.g. sizeof or NULL-checks) compiles; never called at runtime.
 * ---------------------------------------------------------------------- */
/* Declare them as null macros – image loaders never call any qgl* function */
#define qglActiveTextureARB             ((void*)0)
#define qglClientActiveTextureARB       ((void*)0)
#define qglMultiTexCoord2fARB           ((void*)0)
#define qglMultiTexCoord2iARB           ((void*)0)
#define qglLockArraysEXT                ((void*)0)
#define qglUnlockArraysEXT              ((void*)0)
#define qglCreateShaderObjectARB        ((void*)0)
#define qglShaderSourceARB              ((void*)0)
#define qglCompileShaderARB             ((void*)0)
#define qglCreateProgramObjectARB       ((void*)0)
#define qglAttachObjectARB              ((void*)0)
#define qglLinkProgramARB               ((void*)0)
#define qglUseProgramObjectARB          ((void*)0)
#define qglGetObjectParameterivARB      ((void*)0)
#define qglGetInfoLogARB                ((void*)0)
#define qglDetachObjectARB              ((void*)0)
#define qglDeleteObjectARB              ((void*)0)
#define qglGetUniformLocationARB        ((void*)0)
#define qglUniform1fARB                 ((void*)0)
#define qglUniform1iARB                 ((void*)0)
#define qglGenFramebuffersEXT           ((void*)0)
#define qglDeleteFramebuffersEXT        ((void*)0)
#define qglBindFramebufferEXT           ((void*)0)
#define qglFramebufferTexture2DEXT      ((void*)0)
#define qglCheckFramebufferStatusEXT    ((void*)0)
#define qglGenRenderbuffersEXT          ((void*)0)
#define qglDeleteRenderbuffersEXT       ((void*)0)
#define qglBindRenderbufferEXT          ((void*)0)
#define qglFramebufferRenderbufferEXT   ((void*)0)
#define qglRenderbufferStorageEXT       ((void*)0)
#define qglRenderbufferStorageMultisampleEXT ((void*)0)
#define qglBlitFramebufferEXT           ((void*)0)

/* proc typedefs expected by qgl.h consumers */
typedef void (*BindTextureproc)(GLenum, GLuint);
typedef void (*BlendFuncproc)(GLenum, GLenum);
typedef void (*ClearColorproc)(GLclampf, GLclampf, GLclampf, GLclampf);
typedef void (*Clearproc)(GLbitfield);

#endif /* __QGL_H__ */
