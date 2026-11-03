/* dk_device.h -- what the OpenGL layer asks of the thing that really draws.
 *
 * dkgl.c turns the renderer's OpenGL calls into this interface. dk_device.c
 * implements it with deko3D. tools/hostcheck implements it with a software
 * rasterizer, so that all but this last step can run, and be looked at, on a PC.
 */

#ifndef DK_DEVICE_H
#define DK_DEVICE_H

#include <stdint.h>

#include <GL/gl.h>

#define DEV_MAX_TEXTURE_UNITS	2
#define DEV_MAX_TEXTURES	4096	/* tr_local.h's MAX_DRAWIMAGES, and the cinematics' */
#define DEV_MAX_DEPTH_PROBES	256	/* tr_flares.c's MAX_FLARES */

typedef uint64_t devAddr_t;

/* Fragment programs. Each exists with and without the portal's clip plane. */
typedef enum
{
	DEV_FS_COLOR,		/* vertex color */
	DEV_FS_TEX,		/* texture * color */
	DEV_FS_TEX_REPLACE,	/* texture */
	DEV_FS_MULTI_MODULATE,	/* texture * color, times the second texture */
	DEV_FS_MULTI_ADD,	/* the same, plus the second texture */
	DEV_FS_MULTI_DECAL,	/* the same, under the second texture by its alpha */
	DEV_FS_COUNT
} devProgram_t;

/* The vertex program's uniform block, in shaders/rtcw.vert's std140 layout. */
typedef struct
{
	float	mvp[16];
	float	eye[3][4];	/* the modelview's first three rows */
	float	color_const[4];	/* color = array * color_array.x + color_const */
	float	color_array[4];
	float	fog[4];		/* scale, end, density, mode: 0 none, 1 linear, 2 exp, 3 exp2 */
	float	clip_plane[4];	/* eye space */
} devVertexUniforms_t;

/* Everything an OpenGL draw takes from the state that is not a program or a texture. */
typedef struct
{
	GLboolean	blend;
	GLenum		blend_src, blend_dst;
	GLboolean	alpha_test;
	GLenum		alpha_func;
	float		alpha_ref;
	GLboolean	depth_test, depth_write;
	GLenum		depth_func;
	float		depth_near, depth_far;
	GLboolean	cull;
	GLenum		cull_face;
	GLboolean	polygon_offset;
	float		offset_factor, offset_units;
	GLboolean	polygon_line;
	unsigned	color_mask;	/* 1 red, 2 green, 4 blue, 8 alpha */
	GLboolean	stencil_test;
	GLenum		stencil_func;
	int		stencil_ref;
	unsigned	stencil_mask, stencil_write_mask;
	GLenum		stencil_fail, stencil_zfail, stencil_zpass;
	GLboolean	scissor_test;
	int		scissor[4];	/* in OpenGL's terms: y from the bottom */
	int		viewport[4];
} devRenderState_t;

/* What a texture samples with: part of the bind. */
typedef struct
{
	GLenum	min_filter, mag_filter;
	GLenum	wrap_s, wrap_t;
	int	anisotropy;
} devSampler_t;

typedef struct
{
	devAddr_t	pos;	/* 4 floats a vertex, the last unused: the renderer's own layout */
	devAddr_t	color;	/* 4 bytes */
	devAddr_t	st0;	/* 2 floats */
	devAddr_t	st1;
} devVertices_t;

qboolean DEV_Init (int *width, int *height);
void DEV_Shutdown (void);

/* The current frame's memory, for vertices and indices. */
void *DEV_Stream (uint32_t size, devAddr_t *addr);

int DEV_TextureCreate (int width, int height, int levels, qboolean has_alpha);
void DEV_TextureUpload (int id, int level, int x, int y, int width, int height, const void *rgba);
void DEV_TextureDestroy (int id);
void DEV_SetTexture (int unit, int id, const devSampler_t *sampler);

void DEV_SetRenderState (const devRenderState_t *state);
void DEV_SetProgram (devProgram_t program, qboolean clip);
void DEV_SetVertexUniforms (const devVertexUniforms_t *uniforms);
void DEV_SetFogColor (const float *rgba);
void DEV_SetVertices (const devVertices_t *vertices);

void DEV_Draw (GLenum mode, int first, int count);
void DEV_DrawIndexed (GLenum mode, devAddr_t indices, int count);

void DEV_Clear (GLbitfield mask, const float *color, float depth, int stencil);
void DEV_Present (void);

/* The screen as OpenGL gives it: rows from the bottom, tightly packed RGBA. */
void DEV_ReadScreen (int x, int y, int width, int height, void *rgba);

/* Draws the first of the vertices set, and says whether the point drawn at this id
 * some frames ago passed the depth test. */
qboolean DEV_DepthProbe (int id);

#endif	/* DK_DEVICE_H */
