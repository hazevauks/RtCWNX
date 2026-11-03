/* soft_gpu.h -- the rasterizer behind fake_deko3d.c. */

#ifndef SOFT_GPU_H
#define SOFT_GPU_H

#include "dk_device.h"

#define SOFT_MAX_LEVELS	14

typedef struct
{
	qboolean	has_alpha;
	int		levels;
	int		w[SOFT_MAX_LEVELS], h[SOFT_MAX_LEVELS];
	const byte	*pixels[SOFT_MAX_LEVELS];
} softTexture_t;

void SOFT_SetRenderState (const devRenderState_t *state);
void SOFT_SetProgram (devProgram_t program, qboolean clip);
void SOFT_SetVertexUniforms (const devVertexUniforms_t *uniforms);
void SOFT_SetFogColor (const float *rgba);
void SOFT_SetVertices (const devVertices_t *vertices);
void SOFT_BindTexture (int unit, const softTexture_t *texture, const devSampler_t *sampler);

void SOFT_Draw (GLenum mode, int first, int count);
void SOFT_DrawIndexed (GLenum mode, devAddr_t indices, int count);
void SOFT_ClearColor (unsigned mask, const float *color);
void SOFT_ClearDepthStencil (int clear_depth, float depth, unsigned stencil_mask, int stencil);

/* Whether the point about to be drawn passes the depth test: for the flares' queries. */
qboolean SOFT_PointPasses (void);

/* A row of the frame, the first being its top. */
void SOFT_ReadRow (unsigned row, unsigned x, unsigned width, byte *rgba);

/* The present pass: the frame through a table of 256 RGBA entries. */
void SOFT_ApplyGamma (const byte *table);

/* What the screen shows now goes to a picture, if a scenario asked for one. */
void SOFT_Present (void);

/* How many things fake_deko3d.c has refused so far. */
int FakeDeko3D_Errors (void);

#endif	/* SOFT_GPU_H */
