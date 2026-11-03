/* host_device.c -- dk_device.h with a software rasterizer, for a PC.
 *
 * It draws what source/renderer/dkgl.c asks for with the same rules as
 * shaders/rtcw.vert and shaders/rtcw.frag, into memory. It only rasterizes the frames
 * a scenario wants a picture of, so that the others run at full speed, and it
 * counts what every frame asks for, which is what the console's budgets are about.
 *
 * It is here to show that what the renderer asks makes the right picture. What it
 * cannot show is that deko3D does what dk_device.c tells it: that takes a console.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "renderer/tr_local.h"

#include "dk_device.h"
#include "nx_platform.h"
#include "shim/host_device.h"

#define SCREEN_W		1280
#define SCREEN_H		720
#define STREAM_SIZE		(128 * 1024 * 1024)
#define MAX_LEVELS		14
#define MAX_CLIPPED		16

typedef struct
{
	qboolean	used;
	qboolean	has_alpha;
	int		levels;
	int		w[MAX_LEVELS], h[MAX_LEVELS];
	byte		*pixels[MAX_LEVELS];
	size_t		bytes;
} hostTexture_t;

typedef struct
{
	float	clip[4];
	float	color[4];
	float	st0[2], st1[2];
	float	fog;
	float	clip_distance;
} vert_t;

static byte	*color_buffer;
static float	*depth_buffer;
static byte	*stencil_buffer;
static byte	*stream;
static uint32_t	stream_head;

static hostTexture_t	host_textures[DEV_MAX_TEXTURES];
static size_t		texture_bytes;
static int		num_host_textures;

static devRenderState_t		rs;
static devProgram_t		prog;
static qboolean			prog_clip;
static devVertexUniforms_t	vu;
static float			fog_color[4];
static devVertices_t		verts;
static int			bound_texture[DEV_MAX_TEXTURE_UNITS];
static devSampler_t		bound_sampler[DEV_MAX_TEXTURE_UNITS];

static byte	gamma_table[3][256];

static char	shot_name[64];
static char	stats_name[64];
static qboolean	rasterize;

/* what a frame asked for */
static unsigned	c_draws, c_vertices, c_indices, c_clears, c_state_changes, c_texture_binds;
static size_t	c_stream_peak;
static unsigned	c_frames;
static float	c_fragments;

void Host_RequestShot (const char *name)
{
	snprintf(shot_name, sizeof(shot_name), "%s", name);
	rasterize = qtrue;
}

void Host_RequestStats (const char *name)
{
	snprintf(stats_name, sizeof(stats_name), "%s", name);
}

qboolean DEV_Init (int *width, int *height)
{
	if (!color_buffer)
	{
		color_buffer = malloc(SCREEN_W * SCREEN_H * 4);
		depth_buffer = malloc(SCREEN_W * SCREEN_H * sizeof(float));
		stencil_buffer = malloc(SCREEN_W * SCREEN_H);
		stream = malloc(STREAM_SIZE);
	}
	memset(host_textures, 0, sizeof(host_textures));
	texture_bytes = 0;
	num_host_textures = 0;
	*width = SCREEN_W;
	*height = SCREEN_H;
	return qtrue;
}

void DEV_Shutdown (void)
{
	int	i;

	for (i = 0; i < DEV_MAX_TEXTURES; i++)
	{
		if (host_textures[i].used)
			DEV_TextureDestroy(i);
	}
}

void *DEV_Stream (uint32_t size, devAddr_t *addr)
{
	byte	*p;

	stream_head = (stream_head + 15) & ~15u;
	if (stream_head + size > STREAM_SIZE)
	{
		fprintf(stderr, "host: a frame's vertices outgrew %d MB\n", STREAM_SIZE >> 20);
		stream_head = 0;
	}
	p = stream + stream_head;
	stream_head += size;
	*addr = (devAddr_t) p;
	return p;
}

/*
===============================================================================

TEXTURES

===============================================================================
*/

int DEV_TextureCreate (int width, int height, int levels, qboolean has_alpha)
{
	int	id, i;

	for (id = 0; id < DEV_MAX_TEXTURES; id++)
	{
		hostTexture_t	*t = &host_textures[id];

		if (t->used)
			continue;
		t->used = qtrue;
		t->has_alpha = has_alpha;
		t->levels = levels;
		t->bytes = 0;
		for (i = 0; i < levels; i++)
		{
			t->w[i] = width > 1 ? width : 1;
			t->h[i] = height > 1 ? height : 1;
			t->pixels[i] = malloc((size_t) t->w[i] * t->h[i] * 4);
			t->bytes += (size_t) t->w[i] * t->h[i] * 4;
			width >>= 1;
			height >>= 1;
		}
		texture_bytes += t->bytes;
		num_host_textures++;
		return id;
	}
	ri.Error(ERR_FATAL, "host: out of textures");
	return -1;
}

void DEV_TextureUpload (int id, int level, int x, int y, int width, int height, const void *rgba)
{
	hostTexture_t	*t = &host_textures[id];
	const byte	*in = rgba;
	int		row;

	for (row = 0; row < height; row++)
		memcpy(t->pixels[level] + ((size_t) (y + row) * t->w[level] + x) * 4, in + (size_t) row * width * 4, width * 4);
}

void DEV_TextureDestroy (int id)
{
	hostTexture_t	*t = &host_textures[id];
	int		i;

	for (i = 0; i < t->levels; i++)
		free(t->pixels[i]);
	texture_bytes -= t->bytes;
	num_host_textures--;
	memset(t, 0, sizeof(*t));
}

void DEV_SetTexture (int unit, int id, const devSampler_t *sampler)
{
	bound_texture[unit] = id;
	bound_sampler[unit] = *sampler;
	c_texture_binds++;
}

void DEV_SetRenderState (const devRenderState_t *state)
{
	rs = *state;
	c_state_changes++;
}

void DEV_SetProgram (devProgram_t program, qboolean clip)
{
	prog = program;
	prog_clip = clip;
}

void DEV_SetVertexUniforms (const devVertexUniforms_t *uniforms)
{
	vu = *uniforms;
}

void DEV_SetFogColor (const float *rgba)
{
	memcpy(fog_color, rgba, sizeof(fog_color));
}

void DEV_SetVertices (const devVertices_t *vertices)
{
	verts = *vertices;
}

/*
===============================================================================

THE PIPELINE

===============================================================================
*/

static void TransformVertex (int index, vert_t *out)
{
	const float	*p = (const float *) verts.pos + index * 4;
	const float	x = p[0], y = p[1], z = p[2];
	const float	*m = vu.mvp;
	const float	eye_x = vu.eye[0][0] * x + vu.eye[0][1] * y + vu.eye[0][2] * z + vu.eye[0][3];
	const float	eye_y = vu.eye[1][0] * x + vu.eye[1][1] * y + vu.eye[1][2] * z + vu.eye[1][3];
	const float	eye_z = vu.eye[2][0] * x + vu.eye[2][1] * y + vu.eye[2][2] * z + vu.eye[2][3];
	const float	distance = fabsf(eye_z);
	float		f = 1;
	int		i;

	out->clip[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
	out->clip[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
	out->clip[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
	out->clip[3] = m[3] * x + m[7] * y + m[11] * z + m[15];

	for (i = 0; i < 4; i++)
	{
		const float	array = verts.color ? ((const byte *) verts.color)[index * 4 + i] / 255.0f : 0;

		out->color[i] = array * vu.color_array[0] + vu.color_const[i];
	}
	if (verts.st0)
	{
		out->st0[0] = ((const float *) verts.st0)[index * 2];
		out->st0[1] = ((const float *) verts.st0)[index * 2 + 1];
	}
	else
	{
		out->st0[0] = out->st0[1] = 0;
	}
	if (verts.st1)
	{
		out->st1[0] = ((const float *) verts.st1)[index * 2];
		out->st1[1] = ((const float *) verts.st1)[index * 2 + 1];
	}
	else
	{
		out->st1[0] = out->st1[1] = 0;
	}

	if (vu.fog[3] == 1)
		f = (vu.fog[1] - distance) * vu.fog[0];
	else if (vu.fog[3] == 2)
		f = expf(-vu.fog[2] * distance);
	else if (vu.fog[3] == 3)
		f = expf(-vu.fog[2] * distance * vu.fog[2] * distance);
	out->fog = f < 0 ? 0 : (f > 1 ? 1 : f);

	out->clip_distance = eye_x * vu.clip_plane[0] + eye_y * vu.clip_plane[1] + eye_z * vu.clip_plane[2] + vu.clip_plane[3];
}

static void Lerp (const vert_t *a, const vert_t *b, float t, vert_t *out)
{
	int	i;

	for (i = 0; i < 4; i++)
	{
		out->clip[i] = a->clip[i] + (b->clip[i] - a->clip[i]) * t;
		out->color[i] = a->color[i] + (b->color[i] - a->color[i]) * t;
	}
	for (i = 0; i < 2; i++)
	{
		out->st0[i] = a->st0[i] + (b->st0[i] - a->st0[i]) * t;
		out->st1[i] = a->st1[i] + (b->st1[i] - a->st1[i]) * t;
	}
	out->fog = a->fog + (b->fog - a->fog) * t;
	out->clip_distance = a->clip_distance + (b->clip_distance - a->clip_distance) * t;
}

/* Keeps what is on the positive side of a plane through clip space. */
static int ClipPlane (const vert_t *in, int count, vert_t *out, float a, float b, float c, float d)
{
	int	produced = 0;
	int	i;

	for (i = 0; i < count; i++)
	{
		const vert_t	*v0 = &in[i];
		const vert_t	*v1 = &in[(i + 1) % count];
		const float	d0 = a * v0->clip[0] + b * v0->clip[1] + c * v0->clip[2] + d * v0->clip[3];
		const float	d1 = a * v1->clip[0] + b * v1->clip[1] + c * v1->clip[2] + d * v1->clip[3];

		if (d0 >= 0)
			out[produced++] = *v0;
		if ((d0 >= 0) != (d1 >= 0))
			Lerp(v0, v1, d0 / (d0 - d1), &out[produced++]);
	}
	return produced;
}

static void SampleLevel (const hostTexture_t *t, int level, const devSampler_t *smp, float s, float u, float *out)
{
	const int	w = t->w[level], h = t->h[level];
	const byte	*pixels = t->pixels[level];
	const qboolean	linear = smp->mag_filter != GL_NEAREST;
	float		fx = s * w, fy = u * h;
	int		x0, y0, x1, y1;
	float		ax, ay;
	int		i;

	if (linear)
	{
		fx -= 0.5f;
		fy -= 0.5f;
	}
	x0 = (int) floorf(fx);
	y0 = (int) floorf(fy);
	ax = fx - x0;
	ay = fy - y0;
	x1 = x0 + 1;
	y1 = y0 + 1;

	if (smp->wrap_s == GL_REPEAT)
	{
		x0 &= w - 1;
		x1 &= w - 1;
	}
	else
	{
		x0 = x0 < 0 ? 0 : (x0 >= w ? w - 1 : x0);
		x1 = x1 < 0 ? 0 : (x1 >= w ? w - 1 : x1);
	}
	if (smp->wrap_t == GL_REPEAT)
	{
		y0 &= h - 1;
		y1 &= h - 1;
	}
	else
	{
		y0 = y0 < 0 ? 0 : (y0 >= h ? h - 1 : y0);
		y1 = y1 < 0 ? 0 : (y1 >= h ? h - 1 : y1);
	}

	if (!linear)
	{
		const byte	*p = pixels + ((size_t) y0 * w + x0) * 4;

		for (i = 0; i < 4; i++)
			out[i] = p[i] / 255.0f;
	}
	else
	{
		const byte	*p00 = pixels + ((size_t) y0 * w + x0) * 4;
		const byte	*p10 = pixels + ((size_t) y0 * w + x1) * 4;
		const byte	*p01 = pixels + ((size_t) y1 * w + x0) * 4;
		const byte	*p11 = pixels + ((size_t) y1 * w + x1) * 4;

		for (i = 0; i < 4; i++)
		{
			const float	top = p00[i] + (p10[i] - p00[i]) * ax;
			const float	bottom = p01[i] + (p11[i] - p01[i]) * ax;

			out[i] = (top + (bottom - top) * ay) / 255.0f;
		}
	}
	if (!t->has_alpha)
		out[3] = 1;
}

static qboolean Compare (GLenum func, float a, float b)
{
	switch (func)
	{
	case GL_NEVER:		return qfalse;
	case GL_LESS:		return a < b;
	case GL_EQUAL:		return a == b;
	case GL_LEQUAL:		return a <= b;
	case GL_GREATER:	return a > b;
	case GL_NOTEQUAL:	return a != b;
	case GL_GEQUAL:		return a >= b;
	default:		return qtrue;
	}
}

static void BlendFactor (GLenum factor, const float *src, const float *dst, float *out)
{
	int	i;

	for (i = 0; i < 4; i++)
	{
		switch (factor)
		{
		case GL_ZERO:			out[i] = 0; break;
		case GL_ONE:			out[i] = 1; break;
		case GL_SRC_COLOR:		out[i] = src[i]; break;
		case GL_ONE_MINUS_SRC_COLOR:	out[i] = 1 - src[i]; break;
		case GL_DST_COLOR:		out[i] = dst[i]; break;
		case GL_ONE_MINUS_DST_COLOR:	out[i] = 1 - dst[i]; break;
		case GL_SRC_ALPHA:		out[i] = src[3]; break;
		case GL_ONE_MINUS_SRC_ALPHA:	out[i] = 1 - src[3]; break;
		case GL_DST_ALPHA:		out[i] = dst[3]; break;
		case GL_ONE_MINUS_DST_ALPHA:	out[i] = 1 - dst[3]; break;
		case GL_SRC_ALPHA_SATURATE:
		{
			const float	f = src[3] < 1 - dst[3] ? src[3] : 1 - dst[3];

			out[i] = i == 3 ? 1 : f;
			break;
		}
		default:			out[i] = 1; break;
		}
	}
}

static void StencilApply (byte *value, GLenum op)
{
	switch (op)
	{
	case GL_ZERO:		*value = 0; break;
	case GL_REPLACE:	*value = (byte) rs.stencil_ref; break;
	case GL_INCR:		if (*value < 255) (*value)++; break;
	case GL_DECR:		if (*value > 0) (*value)--; break;
	case GL_INVERT:		*value = ~*value; break;
	default:		break;
	}
}

/* Which mip a triangle's texture comes from: one for the triangle, by how many texels
 * it covers. It is a test picture: per pixel mips would add nothing to what it shows. */
static int ChooseLevel (const hostTexture_t *t, const devSampler_t *smp, const float *st_a, const float *st_b, const float *st_c, float screen_area)
{
	float	texel_area, ratio;
	int	level;

	if (t->levels == 1 || smp->min_filter == GL_NEAREST || smp->min_filter == GL_LINEAR)
		return 0;
	texel_area = fabsf((st_b[0] - st_a[0]) * (st_c[1] - st_a[1]) - (st_c[0] - st_a[0]) * (st_b[1] - st_a[1])) * t->w[0] * t->h[0];
	ratio = texel_area / (screen_area > 1e-3f ? screen_area : 1e-3f);
	level = 0;
	while (ratio > 2.0f && level + 1 < t->levels)
	{
		ratio *= 0.25f;
		level++;
	}
	return level;
}

static void RasterTriangle (const vert_t *a, const vert_t *b, const vert_t *c)
{
	const int	*vp = rs.viewport;
	const float	inv_w[3] = { 1.0f / a->clip[3], 1.0f / b->clip[3], 1.0f / c->clip[3] };
	const vert_t	*v[3] = { a, b, c };
	float		wx[3], wy[3], wz[3];
	float		area, inv_area;
	int		min_x, max_x, min_y, max_y;
	const hostTexture_t *tex0 = NULL, *tex1 = NULL;
	int		level0 = 0, level1 = 0;
	float		z_bias = 0;
	int		i, x, y;

	for (i = 0; i < 3; i++)
	{
		wx[i] = vp[0] + (v[i]->clip[0] * inv_w[i] + 1) * 0.5f * vp[2];
		wy[i] = vp[1] + (v[i]->clip[1] * inv_w[i] + 1) * 0.5f * vp[3];
		wz[i] = rs.depth_near + (v[i]->clip[2] * inv_w[i] + 1) * 0.5f * (rs.depth_far - rs.depth_near);
	}

	area = (wx[1] - wx[0]) * (wy[2] - wy[0]) - (wx[2] - wx[0]) * (wy[1] - wy[0]);
	if (area == 0)
		return;
	if (rs.cull)
	{
		const qboolean	front = area > 0;

		if (rs.cull_face == GL_FRONT_AND_BACK || (rs.cull_face == GL_FRONT) == front)
			return;
	}
	inv_area = 1.0f / area;

	min_x = (int) floorf(fminf(fminf(wx[0], wx[1]), wx[2]));
	max_x = (int) ceilf(fmaxf(fmaxf(wx[0], wx[1]), wx[2]));
	min_y = (int) floorf(fminf(fminf(wy[0], wy[1]), wy[2]));
	max_y = (int) ceilf(fmaxf(fmaxf(wy[0], wy[1]), wy[2]));
	if (min_x < vp[0]) min_x = vp[0];
	if (min_y < vp[1]) min_y = vp[1];
	if (max_x > vp[0] + vp[2]) max_x = vp[0] + vp[2];
	if (max_y > vp[1] + vp[3]) max_y = vp[1] + vp[3];
	if (rs.scissor_test)
	{
		if (min_x < rs.scissor[0]) min_x = rs.scissor[0];
		if (min_y < rs.scissor[1]) min_y = rs.scissor[1];
		if (max_x > rs.scissor[0] + rs.scissor[2]) max_x = rs.scissor[0] + rs.scissor[2];
		if (max_y > rs.scissor[1] + rs.scissor[3]) max_y = rs.scissor[1] + rs.scissor[3];
	}
	if (min_x < 0) min_x = 0;
	if (min_y < 0) min_y = 0;
	if (max_x > SCREEN_W) max_x = SCREEN_W;
	if (max_y > SCREEN_H) max_y = SCREEN_H;

	if (prog != DEV_FS_COLOR)
	{
		tex0 = &host_textures[bound_texture[0]];
		level0 = ChooseLevel(tex0, &bound_sampler[0], a->st0, b->st0, c->st0, fabsf(area));
	}
	if (prog == DEV_FS_MULTI_MODULATE || prog == DEV_FS_MULTI_ADD || prog == DEV_FS_MULTI_DECAL)
	{
		tex1 = &host_textures[bound_texture[1]];
		level1 = ChooseLevel(tex1, &bound_sampler[1], a->st1, b->st1, c->st1, fabsf(area));
	}

	if (rs.polygon_offset)
	{
		const float	dzdx = ((wz[1] - wz[0]) * (wy[2] - wy[0]) - (wz[2] - wz[0]) * (wy[1] - wy[0])) * inv_area;
		const float	dzdy = ((wx[1] - wx[0]) * (wz[2] - wz[0]) - (wx[2] - wx[0]) * (wz[1] - wz[0])) * inv_area;

		z_bias = rs.offset_factor * fmaxf(fabsf(dzdx), fabsf(dzdy)) + rs.offset_units / 16777216.0f;
	}

	for (y = min_y; y < max_y; y++)
	{
		const size_t	row = (size_t) (SCREEN_H - 1 - y) * SCREEN_W;

		for (x = min_x; x < max_x; x++)
		{
			const float	px = x + 0.5f, py = y + 0.5f;
			const float	w0 = ((wx[1] - px) * (wy[2] - py) - (wx[2] - px) * (wy[1] - py)) * inv_area;
			const float	w1 = ((wx[2] - px) * (wy[0] - py) - (wx[0] - px) * (wy[2] - py)) * inv_area;
			const float	w2 = 1 - w0 - w1;
			float		persp0, persp1, persp2, inv;
			float		z, frag[4], in_color[4], s0, u0, s1, u1, fog;
			const size_t	offset = row + x;
			int		k;

			if (w0 < 0 || w1 < 0 || w2 < 0)
				continue;

			z = wz[0] * w0 + wz[1] * w1 + wz[2] * w2 + z_bias;
			c_fragments += 1;

			persp0 = w0 * inv_w[0];
			persp1 = w1 * inv_w[1];
			persp2 = w2 * inv_w[2];
			inv = 1.0f / (persp0 + persp1 + persp2);
			persp0 *= inv;
			persp1 *= inv;
			persp2 *= inv;

			if (prog_clip && a->clip_distance * persp0 + b->clip_distance * persp1 + c->clip_distance * persp2 < 0)
				continue;

			for (k = 0; k < 4; k++)
				in_color[k] = a->color[k] * persp0 + b->color[k] * persp1 + c->color[k] * persp2;
			s0 = a->st0[0] * persp0 + b->st0[0] * persp1 + c->st0[0] * persp2;
			u0 = a->st0[1] * persp0 + b->st0[1] * persp1 + c->st0[1] * persp2;
			s1 = a->st1[0] * persp0 + b->st1[0] * persp1 + c->st1[0] * persp2;
			u1 = a->st1[1] * persp0 + b->st1[1] * persp1 + c->st1[1] * persp2;
			fog = a->fog * persp0 + b->fog * persp1 + c->fog * persp2;

			switch (prog)
			{
			case DEV_FS_COLOR:
				memcpy(frag, in_color, sizeof(frag));
				break;
			case DEV_FS_TEX_REPLACE:
				SampleLevel(tex0, level0, &bound_sampler[0], s0, u0, frag);
				break;
			default:
				SampleLevel(tex0, level0, &bound_sampler[0], s0, u0, frag);
				for (k = 0; k < 4; k++)
					frag[k] *= in_color[k];
				break;
			}
			if (tex1)
			{
				float	second[4];

				SampleLevel(tex1, level1, &bound_sampler[1], s1, u1, second);
				if (prog == DEV_FS_MULTI_MODULATE)
				{
					for (k = 0; k < 4; k++)
						frag[k] *= second[k];
				}
				else if (prog == DEV_FS_MULTI_ADD)
				{
					for (k = 0; k < 3; k++)
						frag[k] += second[k];
					frag[3] *= second[3];
				}
				else
				{
					for (k = 0; k < 3; k++)
						frag[k] += (second[k] - frag[k]) * second[3];
				}
			}
			for (k = 0; k < 3; k++)
				frag[k] = fog_color[k] + (frag[k] - fog_color[k]) * fog;

			/* the GPU's own tests and blending */
			if (rs.alpha_test && !Compare(rs.alpha_func, frag[3], rs.alpha_ref))
				continue;

			if (rs.stencil_test)
			{
				byte		*st = &stencil_buffer[offset];
				const unsigned	ref = rs.stencil_ref & rs.stencil_mask, val = *st & rs.stencil_mask;

				if (!Compare(rs.stencil_func, (float) ref, (float) val))
				{
					StencilApply(st, rs.stencil_fail);
					continue;
				}
				if (rs.depth_test && !Compare(rs.depth_func, z, depth_buffer[offset]))
				{
					StencilApply(st, rs.stencil_zfail);
					continue;
				}
				StencilApply(st, rs.stencil_zpass);
			}
			else if (rs.depth_test && !Compare(rs.depth_func, z, depth_buffer[offset]))
			{
				continue;
			}

			if (rs.depth_test && rs.depth_write)
				depth_buffer[offset] = z;

			{
				byte		*out = color_buffer + offset * 4;
				float		result[4];

				if (rs.blend)
				{
					const float	dst[4] = { out[0] / 255.0f, out[1] / 255.0f, out[2] / 255.0f, out[3] / 255.0f };
					float		src_f[4], dst_f[4];

					BlendFactor(rs.blend_src, frag, dst, src_f);
					BlendFactor(rs.blend_dst, frag, dst, dst_f);
					for (k = 0; k < 4; k++)
						result[k] = frag[k] * src_f[k] + dst[k] * dst_f[k];
				}
				else
				{
					memcpy(result, frag, sizeof(result));
				}
				for (k = 0; k < 4; k++)
				{
					if (rs.color_mask & (1u << k))
					{
						const float	value = result[k] < 0 ? 0 : (result[k] > 1 ? 1 : result[k]);

						out[k] = (byte) (value * 255.0f + 0.5f);
					}
				}
			}
		}
	}
}

static void DrawTriangle (int i0, int i1, int i2)
{
	vert_t	poly[3];
	vert_t	buf[3][MAX_CLIPPED];
	int	count = 3;
	int	i;

	TransformVertex(i0, &poly[0]);
	TransformVertex(i1, &poly[1]);
	TransformVertex(i2, &poly[2]);

	count = ClipPlane(poly, count, buf[0], 0, 0, 1, 1);	/* near: z >= -w */
	if (count < 3)
		return;
	count = ClipPlane(buf[0], count, buf[1], 0, 0, -1, 1);	/* far */
	if (count < 3)
		return;
	/* what is behind the eye would divide by nothing */
	count = ClipPlane(buf[1], count, buf[2], 0, 0, 0, 1);
	for (i = 1; i + 1 < count; i++)
		RasterTriangle(&buf[2][0], &buf[2][i], &buf[2][i + 1]);
}

static void DrawPrimitive (GLenum mode, const unsigned *indices, int first, int count)
{
	int	i;

#define INDEX(n) (indices ? (int) indices[n] : first + (n))
	switch (mode)
	{
	case GL_TRIANGLES:
		for (i = 0; i + 2 < count; i += 3)
			DrawTriangle(INDEX(i), INDEX(i + 1), INDEX(i + 2));
		break;
	case GL_TRIANGLE_STRIP:
		for (i = 0; i + 2 < count; i++)
		{
			if (i & 1)
				DrawTriangle(INDEX(i + 1), INDEX(i), INDEX(i + 2));
			else
				DrawTriangle(INDEX(i), INDEX(i + 1), INDEX(i + 2));
		}
		break;
	case GL_TRIANGLE_FAN:
	case GL_POLYGON:
		for (i = 1; i + 1 < count; i++)
			DrawTriangle(INDEX(0), INDEX(i), INDEX(i + 1));
		break;
	case GL_QUADS:
		for (i = 0; i + 3 < count; i += 4)
		{
			DrawTriangle(INDEX(i), INDEX(i + 1), INDEX(i + 2));
			DrawTriangle(INDEX(i), INDEX(i + 2), INDEX(i + 3));
		}
		break;
	default:
		break;	/* lines and points are for debug views */
	}
#undef INDEX
}

void DEV_Draw (GLenum mode, int first, int count)
{
	c_draws++;
	c_vertices += count;
	if (rasterize && !rs.polygon_line)
		DrawPrimitive(mode, NULL, first, count);
}

void DEV_DrawIndexed (GLenum mode, devAddr_t indices, int count)
{
	c_draws++;
	c_indices += count;
	if (rasterize && !rs.polygon_line)
		DrawPrimitive(mode, (const unsigned *) indices, 0, count);
}

void DEV_Clear (GLbitfield mask, const float *color, float depth, int stencil)
{
	int	x, y;
	int	x0 = 0, y0 = 0, x1 = SCREEN_W, y1 = SCREEN_H;

	c_clears++;
	if (!rasterize)
		return;

	if (rs.scissor_test)
	{
		x0 = rs.scissor[0] < 0 ? 0 : rs.scissor[0];
		y0 = rs.scissor[1] < 0 ? 0 : rs.scissor[1];
		x1 = rs.scissor[0] + rs.scissor[2];
		y1 = rs.scissor[1] + rs.scissor[3];
		if (x1 > SCREEN_W) x1 = SCREEN_W;
		if (y1 > SCREEN_H) y1 = SCREEN_H;
	}
	for (y = y0; y < y1; y++)
	{
		const size_t	row = (size_t) (SCREEN_H - 1 - y) * SCREEN_W;

		for (x = x0; x < x1; x++)
		{
			const size_t	offset = row + x;

			if (mask & GL_COLOR_BUFFER_BIT)
			{
				int	k;

				for (k = 0; k < 4; k++)
				{
					if (rs.color_mask & (1u << k))
						color_buffer[offset * 4 + k] = (byte) (color[k] * 255.0f);
				}
			}
			if (mask & GL_DEPTH_BUFFER_BIT)
				depth_buffer[offset] = depth;
			if (mask & GL_STENCIL_BUFFER_BIT)
				stencil_buffer[offset] = (byte) stencil;
		}
	}
}

/* The console only learns this frames later. Saying yes here shows every flare, while
 * the point that the console draws for it still goes through the state this checks. */
qboolean DEV_DepthProbe (int id)
{
	(void) id;
	c_draws++;
	return qtrue;
}

void DEV_SetGammaTable (const unsigned char *red, const unsigned char *green, const unsigned char *blue)
{
	memcpy(gamma_table[0], red, 256);
	memcpy(gamma_table[1], green, 256);
	memcpy(gamma_table[2], blue, 256);
}

void DEV_ReadScreen (int x, int y, int width, int height, void *rgba)
{
	byte	*out = rgba;
	int	row;

	for (row = 0; row < height; row++)
	{
		const size_t	offset = (size_t) (SCREEN_H - 1 - (y + row)) * SCREEN_W + x;

		memcpy(out + (size_t) row * width * 4, color_buffer + offset * 4, width * 4);
	}
}

/*
===============================================================================

OUTPUT

===============================================================================
*/

static void PngChunk (FILE *f, const char *type, const byte *data, uint32_t length)
{
	byte		header[8];
	uLong		crc;

	header[0] = length >> 24; header[1] = length >> 16; header[2] = length >> 8; header[3] = length;
	memcpy(header + 4, type, 4);
	fwrite(header, 1, 8, f);
	if (length)
		fwrite(data, 1, length, f);
	crc = crc32(0, header + 4, 4);
	if (length)
		crc = crc32(crc, data, length);
	header[0] = crc >> 24; header[1] = crc >> 16; header[2] = crc >> 8; header[3] = crc;
	fwrite(header, 1, 4, f);
}

/* Deflate's stored blocks: iortcw's zlib only reads, and a test picture can be large. */
static byte *StoreDeflate (const byte *raw, size_t raw_size, uint32_t *out_size)
{
	const size_t	blocks = raw_size / 65535 + 1;
	byte		*packed = malloc(raw_size + blocks * 5 + 6);
	byte		*out = packed;
	size_t		left = raw_size;
	const uLong	adler = adler32(adler32(0, NULL, 0), raw, raw_size);

	*out++ = 0x78;
	*out++ = 0x01;
	while (left > 0)
	{
		const unsigned	take = left > 65535 ? 65535 : (unsigned) left;

		left -= take;
		*out++ = left == 0;
		*out++ = take & 255;
		*out++ = take >> 8;
		*out++ = ~take & 255;
		*out++ = (~take >> 8) & 255;
		memcpy(out, raw, take);
		raw += take;
		out += take;
	}
	*out++ = adler >> 24;
	*out++ = adler >> 16;
	*out++ = adler >> 8;
	*out++ = adler;
	*out_size = (uint32_t) (out - packed);
	return packed;
}

static void WriteShot (const char *name)
{
	static const byte	signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	const size_t		raw_size = (size_t) SCREEN_H * (1 + SCREEN_W * 3);
	byte			*raw = malloc(raw_size);
	byte			*packed;
	uint32_t		packed_size;
	byte			ihdr[13] = { 0, 0, SCREEN_W >> 8, SCREEN_W & 255, 0, 0, SCREEN_H >> 8, SCREEN_H & 255, 8, 2, 0, 0, 0 };
	char			path[512];
	FILE			*f;
	int			x, y;

	for (y = 0; y < SCREEN_H; y++)
	{
		byte		*out = raw + (size_t) y * (1 + SCREEN_W * 3);
		const byte	*in = color_buffer + (size_t) y * SCREEN_W * 4;

		*out++ = 0;
		for (x = 0; x < SCREEN_W; x++)
		{
			*out++ = gamma_table[0][in[0]];
			*out++ = gamma_table[1][in[1]];
			*out++ = gamma_table[2][in[2]];
			in += 4;
		}
	}
	packed = StoreDeflate(raw, raw_size, &packed_size);

	snprintf(path, sizeof(path), "%s/%s.png", Shim_OutputDir(), name);
	f = fopen(path, "wb");
	if (f)
	{
		fwrite(signature, 1, 8, f);
		PngChunk(f, "IHDR", ihdr, 13);
		PngChunk(f, "IDAT", packed, packed_size);
		PngChunk(f, "IEND", NULL, 0);
		fclose(f);
	}
	free(raw);
	free(packed);
}

void DEV_Present (void)
{
	if (stream_head > c_stream_peak)
		c_stream_peak = stream_head;
	c_frames++;

	if (stats_name[0])
	{
		NX_LogPrintf("stats %s: %u draws, %u indices, %u vertices, %u state changes, %u binds, %u KB of vertices, %d images in %u MB\n",
				stats_name, c_draws, c_indices, c_vertices, c_state_changes, c_texture_binds,
				stream_head / 1024, num_host_textures, (unsigned) (texture_bytes >> 20));
		stats_name[0] = 0;
	}

	if (shot_name[0])
	{
		WriteShot(shot_name);
		NX_LogPrintf("shot %s: %.2f screens of fragments, %u draws\n",
				shot_name, c_fragments / (SCREEN_W * SCREEN_H), c_draws);
		shot_name[0] = 0;
		rasterize = qfalse;
	}

	c_draws = c_vertices = c_indices = c_clears = c_state_changes = c_texture_binds = 0;
	c_fragments = 0;
	stream_head = 0;
}
