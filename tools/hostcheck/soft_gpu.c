/* soft_gpu.c -- a software rasterizer, for tools/hostcheck/fake_deko3d.c to draw with.
 *
 * It draws with the same rules as shaders/rtcw.vert and shaders/rtcw.frag, into memory.
 * It only rasterizes the frames a scenario wants a picture of, so that the others run at
 * full speed, and it counts what every frame asks for, which is what the console's
 * budgets are about.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "qcommon/q_shared.h"

#include "dk_device.h"
#include "nx_platform.h"
#include "soft_gpu.h"
#include "shim/host_scenario.h"

#define SCREEN_W		1280
#define SCREEN_H		720
#define MAX_CLIPPED		16

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

static devRenderState_t		rs;
static devProgram_t		prog;
static qboolean			prog_clip;
static devVertexUniforms_t	vu;
static float			fog_color[4];
static devVertices_t		verts;
static softTexture_t		bound_texture[DEV_MAX_TEXTURE_UNITS];
static qboolean			bound_valid[DEV_MAX_TEXTURE_UNITS];
static devSampler_t		bound_sampler[DEV_MAX_TEXTURE_UNITS];

static char	shot_name[64];
static char	stats_name[64];
static qboolean	rasterize;

/* what a frame asked for */
static unsigned	c_draws, c_vertices, c_indices, c_clears, c_state_changes, c_texture_binds;
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

static void Init (void)
{
	if (color_buffer)
		return;
	color_buffer = calloc(SCREEN_W * SCREEN_H, 4);
	depth_buffer = malloc(SCREEN_W * SCREEN_H * sizeof(float));
	stencil_buffer = calloc(SCREEN_W, SCREEN_H);
}

void SOFT_BindTexture (int unit, const softTexture_t *texture, const devSampler_t *sampler)
{
	c_texture_binds++;
	bound_valid[unit] = texture != NULL;
	if (texture)
	{
		bound_texture[unit] = *texture;
		bound_sampler[unit] = *sampler;
	}
}

void SOFT_SetRenderState (const devRenderState_t *state)
{
	rs = *state;
	c_state_changes++;
}

void SOFT_SetProgram (devProgram_t program, qboolean clip)
{
	prog = program;
	prog_clip = clip;
}

void SOFT_SetVertexUniforms (const devVertexUniforms_t *uniforms)
{
	vu = *uniforms;
}

void SOFT_SetFogColor (const float *rgba)
{
	memcpy(fog_color, rgba, sizeof(fog_color));
}

void SOFT_SetVertices (const devVertices_t *vertices)
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

static void SampleLevel (const softTexture_t *t, int level, const devSampler_t *smp, float s, float u, float *out)
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
static int ChooseLevel (const softTexture_t *t, const devSampler_t *smp, const float *st_a, const float *st_b, const float *st_c, float screen_area)
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
	const softTexture_t *tex0 = NULL, *tex1 = NULL;
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
		if (!bound_valid[0])
			return;
		tex0 = &bound_texture[0];
		level0 = ChooseLevel(tex0, &bound_sampler[0], a->st0, b->st0, c->st0, fabsf(area));
	}
	if (prog == DEV_FS_MULTI_MODULATE || prog == DEV_FS_MULTI_ADD || prog == DEV_FS_MULTI_DECAL)
	{
		if (!bound_valid[1])
			return;
		tex1 = &bound_texture[1];
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

void SOFT_Draw (GLenum mode, int first, int count)
{
	Init();
	c_draws++;
	c_vertices += count;
	if (rasterize && !rs.polygon_line)
		DrawPrimitive(mode, NULL, first, count);
}

void SOFT_DrawIndexed (GLenum mode, devAddr_t indices, int count)
{
	Init();
	c_draws++;
	c_indices += count;
	if (rasterize && !rs.polygon_line)
		DrawPrimitive(mode, (const unsigned *) indices, 0, count);
}

static void ScissorBounds (int *x0, int *y0, int *x1, int *y1)
{
	*x0 = 0;
	*y0 = 0;
	*x1 = SCREEN_W;
	*y1 = SCREEN_H;
	if (rs.scissor_test)
	{
		*x0 = rs.scissor[0] < 0 ? 0 : rs.scissor[0];
		*y0 = rs.scissor[1] < 0 ? 0 : rs.scissor[1];
		*x1 = rs.scissor[0] + rs.scissor[2];
		*y1 = rs.scissor[1] + rs.scissor[3];
		if (*x1 > SCREEN_W) *x1 = SCREEN_W;
		if (*y1 > SCREEN_H) *y1 = SCREEN_H;
	}
}

void SOFT_ClearColor (unsigned mask, const float *color)
{
	int	x0, y0, x1, y1, x, y, k;

	Init();
	c_clears++;
	if (!rasterize)
		return;
	ScissorBounds(&x0, &y0, &x1, &y1);
	for (y = y0; y < y1; y++)
	{
		byte	*row = color_buffer + (size_t) (SCREEN_H - 1 - y) * SCREEN_W * 4;

		for (x = x0; x < x1; x++)
			for (k = 0; k < 4; k++)
				if (mask & (1u << k))
					row[x * 4 + k] = (byte) (color[k] * 255.0f);
	}
}

void SOFT_ClearDepthStencil (int clear_depth, float depth, unsigned stencil_mask, int stencil)
{
	int	x0, y0, x1, y1, x, y;

	Init();
	c_clears++;
	if (!rasterize)
		return;
	ScissorBounds(&x0, &y0, &x1, &y1);
	for (y = y0; y < y1; y++)
	{
		const size_t	row = (size_t) (SCREEN_H - 1 - y) * SCREEN_W;

		for (x = x0; x < x1; x++)
		{
			if (clear_depth)
				depth_buffer[row + x] = depth;
			if (stencil_mask)
				stencil_buffer[row + x] = (byte) stencil;
		}
	}
}

/* Frames that are not drawn have no depth to test against. */
qboolean SOFT_PointPasses (void)
{
	vert_t	v;
	float	wx, wy, wz;
	int	x, y;

	Init();
	if (!rasterize)
		return qtrue;
	TransformVertex(0, &v);
	wx = rs.viewport[0] + (v.clip[0] / v.clip[3] + 1) * 0.5f * rs.viewport[2];
	wy = rs.viewport[1] + (v.clip[1] / v.clip[3] + 1) * 0.5f * rs.viewport[3];
	wz = rs.depth_near + (v.clip[2] / v.clip[3] + 1) * 0.5f * (rs.depth_far - rs.depth_near);
	x = (int) wx;
	y = (int) wy;
	if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H)
		return qfalse;
	return Compare(rs.depth_func, wz, depth_buffer[(size_t) (SCREEN_H - 1 - y) * SCREEN_W + x]);
}

void SOFT_ReadRow (unsigned row, unsigned x, unsigned width, byte *rgba)
{
	Init();
	memcpy(rgba, color_buffer + ((size_t) row * SCREEN_W + x) * 4, width * 4);
}

void SOFT_ApplyGamma (const byte *table)
{
	size_t	i;

	Init();
	if (!rasterize)
		return;
	for (i = 0; i < (size_t) SCREEN_W * SCREEN_H; i++)
	{
		byte	*p = color_buffer + i * 4;

		p[0] = table[p[0] * 4 + 0];
		p[1] = table[p[1] * 4 + 1];
		p[2] = table[p[2] * 4 + 2];
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
			*out++ = in[0];
			*out++ = in[1];
			*out++ = in[2];
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

void SOFT_Present (void)
{
	Init();
	if (stats_name[0])
	{
		NX_LogPrintf("stats %s: %u draws, %u indices, %u vertices, %u state changes, %u binds, %d fake deko3D errors\n",
				stats_name, c_draws, c_indices, c_vertices, c_state_changes, c_texture_binds, FakeDeko3D_Errors());
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
}
