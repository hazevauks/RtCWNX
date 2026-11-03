/* dkgl.c -- the part of OpenGL that iortcw's renderer uses, without a driver.
 *
 * The renderer (the same code on PC) does its work on the CPU and hands OpenGL
 * arrays and state: fixed function, texture environments, hardware fog. This keeps
 * that state, and when something is drawn, passes on what changed to dk_device.h:
 * one vertex program, a few fragment programs chosen by the texture environment,
 * and vertices copied to the frame's memory, since the renderer rewrites its own
 * arrays between draws. See docs/renderer.md.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "renderer/tr_local.h"

#include "dk_device.h"
#include "dkgl.h"

#define MATRIX_STACK_DEPTH	8
#define IMMEDIATE_VERTICES	(SHADER_MAX_VERTEXES + 64)
#define MAX_TEXTURE_LEVELS	14

enum
{
	DIRTY_STATE	= 1 << 0,
	DIRTY_PROGRAM	= 1 << 1,
	DIRTY_UNIFORMS	= 1 << 2,
	DIRTY_MVP	= 1 << 3,
	DIRTY_TEXTURE0	= 1 << 4,
	DIRTY_TEXTURE1	= 1 << 5,
	DIRTY_FOG_COLOR	= 1 << 6,
	DIRTY_ALL	= (1 << 7) - 1
};

typedef struct
{
	const byte	*pointer;
	GLint		size;
	GLenum		type;
	GLsizei		stride;
	GLboolean	enabled;
} clientArray_t;

typedef struct
{
	int		device_id;	/* -1 until there is something to show */
	devSampler_t	sampler;
	int		width, height;
	qboolean	has_alpha;
	/* What arrived, until the mip count is known: the first draw or change. */
	byte		*pending[MAX_TEXTURE_LEVELS];
	int		pending_w[MAX_TEXTURE_LEVELS];
	int		pending_h[MAX_TEXTURE_LEVELS];
	int		pending_levels;
} dkTexture_t;

static devRenderState_t		state;
static devVertexUniforms_t	uniforms;
static unsigned			dirty;

static float	modelview[MATRIX_STACK_DEPTH][16];
static float	projection[MATRIX_STACK_DEPTH][16];
static int	modelview_top, projection_top;
static GLenum	matrix_mode = GL_MODELVIEW;

/* Texture units */
static int		active_unit;
static int		client_unit;
static GLboolean	unit_enabled[DEV_MAX_TEXTURE_UNITS];
static GLuint		unit_texture[DEV_MAX_TEXTURE_UNITS];
static GLenum		unit_env[DEV_MAX_TEXTURE_UNITS];
static clientArray_t	array_st[DEV_MAX_TEXTURE_UNITS];
static GLboolean	clip_enabled;

/* Arrays */
static clientArray_t	array_pos;
static clientArray_t	array_color;

/* The current values of what has no array */
static float		current_color[4] = { 1, 1, 1, 1 };
static byte		current_color_b[4] = { 255, 255, 255, 255 };
static float		current_st[DEV_MAX_TEXTURE_UNITS][2];

static GLboolean	fog_enabled;
static GLenum		fog_mode = GL_EXP;
static float		fog_start, fog_end = 1, fog_density = 1;
static float		fog_color[4];
static float		clear_color[4];
static float		clear_depth = 1;
static int		clear_stencil;

/* Compiled vertex arrays: what the renderer promises not to change. */
static int		lock_count;
static devAddr_t	lock_pos_addr;
static const void	*lock_indices;
static int		lock_indices_count;
static devAddr_t	lock_indices_addr;

/* Immediate mode */
static GLenum		imm_mode;
static int		imm_count;
static float		(*imm_pos)[4];
static byte		(*imm_color)[4];
static float		(*imm_st[DEV_MAX_TEXTURE_UNITS])[2];

static dkTexture_t	textures[DEV_MAX_TEXTURES];
static GLuint		next_texture_name = 1;

/*
===============================================================================

MATRICES

===============================================================================
*/

static float *CurrentMatrix (void)
{
	return matrix_mode == GL_PROJECTION ? projection[projection_top] : modelview[modelview_top];
}

static void MatrixChanged (void)
{
	dirty |= DIRTY_MVP;
}

static void Multiply (float *out, const float *a, const float *b)
{
	int	i, j;

	for (i = 0; i < 4; i++)
	{
		for (j = 0; j < 4; j++)
		{
			out[i * 4 + j] = b[i * 4 + 0] * a[0 * 4 + j] + b[i * 4 + 1] * a[1 * 4 + j]
					+ b[i * 4 + 2] * a[2 * 4 + j] + b[i * 4 + 3] * a[3 * 4 + j];
		}
	}
}

static void MultiplyCurrent (const float *m)
{
	float	*current = CurrentMatrix();
	float	result[16];

	Multiply(result, current, m);
	memcpy(current, result, sizeof(result));
	MatrixChanged();
}

static void APIENTRY dkglMatrixMode (GLenum mode)
{
	matrix_mode = mode;
}

static void APIENTRY dkglLoadIdentity (void)
{
	float	*m = CurrentMatrix();

	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1;
	MatrixChanged();
}

static void APIENTRY dkglLoadMatrixf (const GLfloat *m)
{
	memcpy(CurrentMatrix(), m, 16 * sizeof(float));
	MatrixChanged();
}

static void APIENTRY dkglPushMatrix (void)
{
	if (matrix_mode == GL_PROJECTION)
	{
		if (projection_top + 1 < MATRIX_STACK_DEPTH)
		{
			memcpy(projection[projection_top + 1], projection[projection_top], sizeof(projection[0]));
			projection_top++;
		}
	}
	else if (modelview_top + 1 < MATRIX_STACK_DEPTH)
	{
		memcpy(modelview[modelview_top + 1], modelview[modelview_top], sizeof(modelview[0]));
		modelview_top++;
	}
}

static void APIENTRY dkglPopMatrix (void)
{
	if (matrix_mode == GL_PROJECTION)
	{
		if (projection_top > 0)
			projection_top--;
	}
	else if (modelview_top > 0)
	{
		modelview_top--;
	}
	MatrixChanged();
}

static void APIENTRY dkglTranslatef (GLfloat x, GLfloat y, GLfloat z)
{
	float	m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

	m[12] = x;
	m[13] = y;
	m[14] = z;
	MultiplyCurrent(m);
}

static void APIENTRY dkglOrtho (GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val)
{
	float	m[16] = { 0 };

	m[0] = 2 / (right - left);
	m[5] = 2 / (top - bottom);
	m[10] = -2 / (far_val - near_val);
	m[12] = -(right + left) / (right - left);
	m[13] = -(top + bottom) / (top - bottom);
	m[14] = -(far_val + near_val) / (far_val - near_val);
	m[15] = 1;
	MultiplyCurrent(m);
}

static void APIENTRY dkglFrustum (GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val)
{
	float	m[16] = { 0 };

	m[0] = 2 * near_val / (right - left);
	m[5] = 2 * near_val / (top - bottom);
	m[8] = (right + left) / (right - left);
	m[9] = (top + bottom) / (top - bottom);
	m[10] = -(far_val + near_val) / (far_val - near_val);
	m[11] = -1;
	m[14] = -2 * far_val * near_val / (far_val - near_val);
	MultiplyCurrent(m);
}

/* OpenGL keeps a clip plane in eye space, by the modelview's inverse transpose. */
static qboolean Invert (const float *m, float *out)
{
	float	inv[16], det;
	int	i;

	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det == 0)
		return qfalse;
	det = 1.0f / det;
	for (i = 0; i < 16; i++)
		out[i] = inv[i] * det;
	return qtrue;
}

static void APIENTRY dkglClipPlane (GLenum plane, const GLdouble *equation)
{
	float	inverse[16];
	int	i;

	(void) plane;
	if (!Invert(modelview[modelview_top], inverse))
		return;
	for (i = 0; i < 4; i++)
	{
		uniforms.clip_plane[i] = equation[0] * inverse[i * 4 + 0] + equation[1] * inverse[i * 4 + 1]
				+ equation[2] * inverse[i * 4 + 2] + equation[3] * inverse[i * 4 + 3];
	}
	dirty |= DIRTY_UNIFORMS;
}

/*
===============================================================================

STATE

===============================================================================
*/

static void UpdateFog (void)
{
	if (!fog_enabled)
	{
		uniforms.fog[3] = 0;
	}
	else if (fog_mode == GL_LINEAR)
	{
		uniforms.fog[0] = fog_end != fog_start ? 1.0f / (fog_end - fog_start) : 0;
		uniforms.fog[1] = fog_end;
		uniforms.fog[3] = 1;
	}
	else
	{
		uniforms.fog[2] = fog_density;
		uniforms.fog[3] = fog_mode == GL_EXP ? 2 : 3;
	}
	dirty |= DIRTY_UNIFORMS;
}

static void SetCapability (GLenum cap, GLboolean value)
{
	switch (cap)
	{
	case GL_BLEND:			state.blend = value; dirty |= DIRTY_STATE; break;
	case GL_ALPHA_TEST:		state.alpha_test = value; dirty |= DIRTY_STATE; break;
	case GL_DEPTH_TEST:		state.depth_test = value; dirty |= DIRTY_STATE; break;
	case GL_CULL_FACE:		state.cull = value; dirty |= DIRTY_STATE; break;
	case GL_STENCIL_TEST:		state.stencil_test = value; dirty |= DIRTY_STATE; break;
	case GL_SCISSOR_TEST:		state.scissor_test = value; dirty |= DIRTY_STATE; break;
	case GL_POLYGON_OFFSET_FILL:	state.polygon_offset = value; dirty |= DIRTY_STATE; break;
	case GL_CLIP_PLANE0:		clip_enabled = value; dirty |= DIRTY_PROGRAM; break;
	case GL_FOG:			fog_enabled = value; UpdateFog(); break;
	case GL_TEXTURE_2D:
		if (unit_enabled[active_unit] != value)
		{
			unit_enabled[active_unit] = value;
			dirty |= DIRTY_PROGRAM | (DIRTY_TEXTURE0 << active_unit);
		}
		break;
	default:
		break;
	}
}

static void APIENTRY dkglEnable (GLenum cap)
{
	SetCapability(cap, GL_TRUE);
}

static void APIENTRY dkglDisable (GLenum cap)
{
	SetCapability(cap, GL_FALSE);
}

static GLboolean APIENTRY dkglIsEnabled (GLenum cap)
{
	switch (cap)
	{
	case GL_COLOR_ARRAY:		return array_color.enabled;
	case GL_TEXTURE_COORD_ARRAY:	return array_st[client_unit].enabled;
	case GL_VERTEX_ARRAY:		return array_pos.enabled;
	case GL_TEXTURE_2D:		return unit_enabled[active_unit];
	case GL_BLEND:			return state.blend;
	case GL_ALPHA_TEST:		return state.alpha_test;
	case GL_DEPTH_TEST:		return state.depth_test;
	case GL_CULL_FACE:		return state.cull;
	case GL_FOG:			return fog_enabled;
	default:			return GL_FALSE;
	}
}

static void SetClientState (GLenum cap, GLboolean value)
{
	switch (cap)
	{
	case GL_VERTEX_ARRAY:		array_pos.enabled = value; break;
	case GL_COLOR_ARRAY:		array_color.enabled = value; break;
	case GL_TEXTURE_COORD_ARRAY:	array_st[client_unit].enabled = value; break;
	default:			break;
	}
}

static void APIENTRY dkglEnableClientState (GLenum cap)
{
	SetClientState(cap, GL_TRUE);
}

static void APIENTRY dkglDisableClientState (GLenum cap)
{
	SetClientState(cap, GL_FALSE);
}

static void APIENTRY dkglVertexPointer (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
	array_pos.size = size;
	array_pos.type = type;
	array_pos.stride = stride ? stride : size * sizeof(float);
	array_pos.pointer = ptr;
}

static void APIENTRY dkglColorPointer (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
	array_color.size = size;
	array_color.type = type;
	array_color.stride = stride ? stride : size;
	array_color.pointer = ptr;
}

static void APIENTRY dkglTexCoordPointer (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
	clientArray_t	*a = &array_st[client_unit];

	a->size = size;
	a->type = type;
	a->stride = stride ? stride : size * sizeof(float);
	a->pointer = ptr;
}

static void APIENTRY dkglNormalPointer (GLenum type, GLsizei stride, const GLvoid *ptr)
{
	/* only the ATI tessellation this port has no use for reads normals */
	(void) type;
	(void) stride;
	(void) ptr;
}

static void APIENTRY dkglActiveTextureARB (GLenum texture)
{
	active_unit = texture - GL_TEXTURE0_ARB;
}

static void APIENTRY dkglClientActiveTextureARB (GLenum texture)
{
	client_unit = texture - GL_TEXTURE0_ARB;
}

static void APIENTRY dkglAlphaFunc (GLenum func, GLclampf ref)
{
	state.alpha_func = func;
	state.alpha_ref = ref;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglBlendFunc (GLenum sfactor, GLenum dfactor)
{
	state.blend_src = sfactor;
	state.blend_dst = dfactor;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglColorMask (GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	state.color_mask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglCullFace (GLenum mode)
{
	state.cull_face = mode;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglDepthFunc (GLenum func)
{
	state.depth_func = func;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglDepthMask (GLboolean flag)
{
	state.depth_write = flag;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglDepthRange (GLclampd near_val, GLclampd far_val)
{
	state.depth_near = near_val;
	state.depth_far = far_val;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglPolygonMode (GLenum face, GLenum mode)
{
	(void) face;
	state.polygon_line = mode == GL_LINE;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglPolygonOffset (GLfloat factor, GLfloat units)
{
	state.offset_factor = factor;
	state.offset_units = units;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglScissor (GLint x, GLint y, GLsizei width, GLsizei height)
{
	state.scissor[0] = x;
	state.scissor[1] = y;
	state.scissor[2] = width;
	state.scissor[3] = height;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglViewport (GLint x, GLint y, GLsizei width, GLsizei height)
{
	state.viewport[0] = x;
	state.viewport[1] = y;
	state.viewport[2] = width;
	state.viewport[3] = height;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglStencilFunc (GLenum func, GLint ref, GLuint mask)
{
	state.stencil_func = func;
	state.stencil_ref = ref;
	state.stencil_mask = mask;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglStencilMask (GLuint mask)
{
	state.stencil_write_mask = mask;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglStencilOp (GLenum fail, GLenum zfail, GLenum zpass)
{
	state.stencil_fail = fail;
	state.stencil_zfail = zfail;
	state.stencil_zpass = zpass;
	dirty |= DIRTY_STATE;
}

static void APIENTRY dkglFogf (GLenum pname, GLfloat param)
{
	switch (pname)
	{
	case GL_FOG_START:	fog_start = param; break;
	case GL_FOG_END:	fog_end = param; break;
	case GL_FOG_DENSITY:	fog_density = param; break;
	case GL_FOG_MODE:	fog_mode = (GLenum) param; break;
	default:		return;
	}
	UpdateFog();
}

static void APIENTRY dkglFogi (GLenum pname, GLint param)
{
	dkglFogf(pname, (GLfloat) param);
}

static void APIENTRY dkglFogfv (GLenum pname, const GLfloat *params)
{
	if (pname == GL_FOG_COLOR)
	{
		memcpy(fog_color, params, sizeof(fog_color));
		dirty |= DIRTY_FOG_COLOR;
	}
	else
	{
		dkglFogf(pname, params[0]);
	}
}

static void APIENTRY dkglTexEnvf (GLenum target, GLenum pname, GLfloat param)
{
	(void) target;
	if (pname == GL_TEXTURE_ENV_MODE)
	{
		unit_env[active_unit] = (GLenum) param;
		dirty |= DIRTY_PROGRAM;
	}
}

static void APIENTRY dkglClearColor (GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	clear_color[0] = r;
	clear_color[1] = g;
	clear_color[2] = b;
	clear_color[3] = a;
}

static void APIENTRY dkglClearDepth (GLclampd depth)
{
	clear_depth = depth;
}

static void APIENTRY dkglClearStencil (GLint s)
{
	clear_stencil = s;
}

static void APIENTRY dkglHint (GLenum target, GLenum mode)
{
	(void) target;
	(void) mode;
}

static void APIENTRY dkglShadeModel (GLenum mode)
{
	(void) mode;	/* flat is for debug views only */
}

static void APIENTRY dkglLineWidth (GLfloat width)
{
	(void) width;
}

static void APIENTRY dkglDrawBuffer (GLenum mode)
{
	(void) mode;
}

static void APIENTRY dkglPixelStorei (GLenum pname, GLint param)
{
	(void) pname;
	(void) param;
}

/* The renderer asks for this to pace itself on a driver that queues up frames.
 * Here the swapchain does that, and waiting would only cost parallelism. */
static void APIENTRY dkglFinish (void)
{
}

static void APIENTRY dkglFlush (void)
{
}

static GLenum APIENTRY dkglGetError (void)
{
	return GL_NO_ERROR;
}

static void APIENTRY dkglGetIntegerv (GLenum pname, GLint *params)
{
	switch (pname)
	{
	case GL_MAX_TEXTURE_SIZE:	*params = 2048; break;
	case GL_MAX_TEXTURE_UNITS_ARB:	*params = DEV_MAX_TEXTURE_UNITS; break;
	case GL_PACK_ALIGNMENT:		*params = 1; break;
	case GL_UNPACK_ALIGNMENT:	*params = 1; break;
	case GL_STENCIL_BITS:		*params = 8; break;
	case GL_DEPTH_BITS:		*params = 24; break;
	default:			*params = 0; break;
	}
}

static void APIENTRY dkglGetBooleanv (GLenum pname, GLboolean *params)
{
	(void) pname;
	*params = GL_FALSE;
}

static const GLubyte *APIENTRY dkglGetString (GLenum name)
{
	switch (name)
	{
	case GL_VENDOR:		return (const GLubyte *) "RtCWNX";
	case GL_RENDERER:	return (const GLubyte *) "deko3D";
	case GL_VERSION:	return (const GLubyte *) "1.3 deko3D";
	case GL_EXTENSIONS:	return (const GLubyte *) "GL_ARB_multitexture GL_EXT_compiled_vertex_array GL_EXT_texture_env_add";
	default:		return (const GLubyte *) "";
	}
}

/*
===============================================================================

TEXTURES

===============================================================================
*/

static void FreePending (dkTexture_t *t)
{
	int	i;

	for (i = 0; i < t->pending_levels; i++)
	{
		free(t->pending[i]);
		t->pending[i] = NULL;
	}
	t->pending_levels = 0;
}

/* The renderer sends a texture's levels one by one and never says how many. */
static void FinishTexture (dkTexture_t *t)
{
	int	i;

	if (!t->pending_levels)
		return;

	if (t->device_id >= 0)
		DEV_TextureDestroy(t->device_id);
	t->device_id = DEV_TextureCreate(t->pending_w[0], t->pending_h[0], t->pending_levels, t->has_alpha);
	for (i = 0; i < t->pending_levels; i++)
		DEV_TextureUpload(t->device_id, i, 0, 0, t->pending_w[i], t->pending_h[i], t->pending[i]);
	FreePending(t);
}

static dkTexture_t *ActiveTexture (void)
{
	const GLuint	name = unit_texture[active_unit];

	return name < DEV_MAX_TEXTURES ? &textures[name] : NULL;
}

static void APIENTRY dkglGenTextures (GLsizei n, GLuint *names)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
		names[i] = next_texture_name++ % DEV_MAX_TEXTURES;
}

static void APIENTRY dkglDeleteTextures (GLsizei n, const GLuint *names)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
	{
		dkTexture_t	*t;

		if (names[i] >= DEV_MAX_TEXTURES)
			continue;
		t = &textures[names[i]];
		FreePending(t);
		if (t->device_id >= 0)
			DEV_TextureDestroy(t->device_id);
		t->device_id = -1;
		if (unit_texture[0] == names[i])
			dirty |= DIRTY_TEXTURE0;
		if (unit_texture[1] == names[i])
			dirty |= DIRTY_TEXTURE1;
	}
}

static void APIENTRY dkglBindTexture (GLenum target, GLuint texture)
{
	(void) target;
	if (unit_texture[active_unit] != texture)
	{
		unit_texture[active_unit] = texture;
		dirty |= DIRTY_TEXTURE0 << active_unit;
	}
}

static qboolean FormatHasAlpha (GLint internal_format)
{
	switch (internal_format)
	{
	case 3:
	case GL_RGB:
	case GL_RGB8:
	case GL_RGB5:
	case GL_RGB4:
	case GL_LUMINANCE:
		return qfalse;
	default:
		return qtrue;
	}
}

static void APIENTRY dkglTexImage2D (GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
		GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
	dkTexture_t	*t = ActiveTexture();
	const size_t	size = (size_t) width * height * 4;
	byte		*copy;

	(void) target;
	(void) border;
	if (!t || level >= MAX_TEXTURE_LEVELS || format != GL_RGBA || type != GL_UNSIGNED_BYTE)
		return;

	if (level == 0)
	{
		FreePending(t);
		t->width = width;
		t->height = height;
		t->has_alpha = FormatHasAlpha(internal_format);
	}
	if (level != t->pending_levels)
		return;

	copy = malloc(size);
	if (!copy)
		ri.Error(ERR_FATAL, "dkgl: out of memory for a %dx%d texture", width, height);
	if (pixels)
		memcpy(copy, pixels, size);
	else
		memset(copy, 0, size);
	t->pending[level] = copy;
	t->pending_w[level] = width;
	t->pending_h[level] = height;
	t->pending_levels = level + 1;
	dirty |= DIRTY_TEXTURE0 << active_unit;
}

static void APIENTRY dkglTexSubImage2D (GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
		GLenum format, GLenum type, const GLvoid *pixels)
{
	dkTexture_t	*t = ActiveTexture();

	(void) target;
	if (!t || format != GL_RGBA || type != GL_UNSIGNED_BYTE)
		return;
	FinishTexture(t);
	if (t->device_id >= 0)
		DEV_TextureUpload(t->device_id, level, x, y, width, height, pixels);
}

static void APIENTRY dkglCopyTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
	/* only tr_bloom.c copies the screen, and the port keeps it off */
	(void) target;
	(void) level;
	(void) xoffset;
	(void) yoffset;
	(void) x;
	(void) y;
	(void) width;
	(void) height;
}

static void SetTextureParameter (GLenum pname, int value)
{
	dkTexture_t	*t = ActiveTexture();

	if (!t)
		return;
	switch (pname)
	{
	case GL_TEXTURE_MIN_FILTER:	t->sampler.min_filter = value; break;
	case GL_TEXTURE_MAG_FILTER:	t->sampler.mag_filter = value; break;
	case GL_TEXTURE_WRAP_S:		t->sampler.wrap_s = value; break;
	case GL_TEXTURE_WRAP_T:		t->sampler.wrap_t = value; break;
	case GL_TEXTURE_MAX_ANISOTROPY_EXT: t->sampler.anisotropy = value; break;
	default:			return;
	}
	dirty |= DIRTY_TEXTURE0 << active_unit;
}

static void APIENTRY dkglTexParameterf (GLenum target, GLenum pname, GLfloat param)
{
	(void) target;
	SetTextureParameter(pname, (int) param);
}

static void APIENTRY dkglTexParameteri (GLenum target, GLenum pname, GLint param)
{
	(void) target;
	SetTextureParameter(pname, param);
}

/*
===============================================================================

DRAWING

===============================================================================
*/

/* Which fragment program the two units' state comes to. */
static devProgram_t ChooseProgram (void)
{
	const GLboolean	tex0 = unit_enabled[0] && textures[unit_texture[0] % DEV_MAX_TEXTURES].device_id >= 0;
	const GLboolean	tex1 = unit_enabled[1] && textures[unit_texture[1] % DEV_MAX_TEXTURES].device_id >= 0;

	if (tex0 && tex1)
	{
		switch (unit_env[1])
		{
		case GL_ADD:	return DEV_FS_MULTI_ADD;
		case GL_DECAL:	return DEV_FS_MULTI_DECAL;
		default:	return DEV_FS_MULTI_MODULATE;
		}
	}
	if (tex0)
		return unit_env[0] == GL_REPLACE ? DEV_FS_TEX_REPLACE : DEV_FS_TEX;
	return DEV_FS_COLOR;
}

static void BindUnit (int unit)
{
	if (unit_enabled[unit])
	{
		dkTexture_t	*t = &textures[unit_texture[unit] % DEV_MAX_TEXTURES];

		FinishTexture(t);
		if (t->device_id >= 0)
			DEV_SetTexture(unit, t->device_id, &t->sampler);
	}
}

/* Passes on what changed since the last draw. */
static void FlushState (void)
{
	if (dirty & DIRTY_TEXTURE0)
		BindUnit(0);
	if (dirty & DIRTY_TEXTURE1)
		BindUnit(1);
	if (dirty & (DIRTY_TEXTURE0 | DIRTY_TEXTURE1 | DIRTY_PROGRAM))
		DEV_SetProgram(ChooseProgram(), clip_enabled);
	if (dirty & DIRTY_STATE)
		DEV_SetRenderState(&state);
	if (dirty & DIRTY_FOG_COLOR)
		DEV_SetFogColor(fog_color);
	if (dirty & DIRTY_MVP)
	{
		const float	*mv = modelview[modelview_top];
		int		i;

		Multiply(uniforms.mvp, projection[projection_top], mv);
		for (i = 0; i < 3; i++)
		{
			uniforms.eye[i][0] = mv[0 + i];
			uniforms.eye[i][1] = mv[4 + i];
			uniforms.eye[i][2] = mv[8 + i];
			uniforms.eye[i][3] = mv[12 + i];
		}
		dirty |= DIRTY_UNIFORMS;
	}
	if (dirty & DIRTY_UNIFORMS)
		DEV_SetVertexUniforms(&uniforms);
	dirty = 0;
}

static void SetConstantColor (const float *value, float array_scale)
{
	if (uniforms.color_array[0] != array_scale || memcmp(uniforms.color_const, value, sizeof(uniforms.color_const)))
	{
		uniforms.color_array[0] = array_scale;
		memcpy(uniforms.color_const, value, sizeof(uniforms.color_const));
		dirty |= DIRTY_UNIFORMS;
	}
}

static devAddr_t CopyPositions (int first, int count)
{
	devAddr_t	addr;
	float		*out = DEV_Stream(count * 4 * sizeof(float), &addr);
	const byte	*in = array_pos.pointer + first * array_pos.stride;
	int		i;

	if (array_pos.stride == 4 * sizeof(float))
	{
		memcpy(out, in, count * 4 * sizeof(float));
		return addr;
	}
	for (i = 0; i < count; i++)
	{
		const float	*p = (const float *) in;

		out[0] = p[0];
		out[1] = p[1];
		out[2] = array_pos.size >= 3 ? p[2] : 0;
		out[3] = 1;
		out += 4;
		in += array_pos.stride;
	}
	return addr;
}

static devAddr_t CopyColors (int first, int count)
{
	devAddr_t	addr;
	byte		*out = DEV_Stream(count * 4, &addr);
	const byte	*in = array_color.pointer + first * array_color.stride;
	int		i;

	if (array_color.stride == 4)
	{
		memcpy(out, in, count * 4);
		return addr;
	}
	for (i = 0; i < count; i++)
	{
		memcpy(out, in, 4);
		out += 4;
		in += array_color.stride;
	}
	return addr;
}

static devAddr_t CopyTexCoords (int unit, int first, int count)
{
	const clientArray_t	*a = &array_st[unit];
	devAddr_t		addr;
	float			*out = DEV_Stream(count * 2 * sizeof(float), &addr);
	const byte		*in = a->pointer + first * a->stride;
	int			i;

	if (a->stride == 2 * sizeof(float))
	{
		memcpy(out, in, count * 2 * sizeof(float));
		return addr;
	}
	for (i = 0; i < count; i++)
	{
		const float	*st = (const float *) in;

		out[0] = st[0];
		out[1] = st[1];
		out += 2;
		in += a->stride;
	}
	return addr;
}

/* A unit with no array reads one value for all its vertices. */
static devAddr_t RepeatTexCoord (int unit, int count)
{
	devAddr_t	addr;
	float		*out = DEV_Stream(count * 2 * sizeof(float), &addr);
	int		i;

	for (i = 0; i < count; i++)
	{
		out[0] = current_st[unit][0];
		out[1] = current_st[unit][1];
		out += 2;
	}
	return addr;
}

/* Copies what the draw reads, up to vertex first + count, to the frame's memory. */
static void SetupArrays (int first, int count)
{
	static const float	no_color[4] = { 0, 0, 0, 0 };
	devVertices_t		v;

	if (lock_count && first == 0 && count <= lock_count)
	{
		if (!lock_pos_addr)
			lock_pos_addr = CopyPositions(0, lock_count);
		v.pos = lock_pos_addr;
	}
	else
	{
		v.pos = CopyPositions(first, count);
	}

	if (array_color.enabled)
	{
		v.color = CopyColors(first, count);
		SetConstantColor(no_color, 1.0f);
	}
	else
	{
		v.color = 0;
		SetConstantColor(current_color, 0.0f);
	}

	if (unit_enabled[0])
		v.st0 = array_st[0].enabled ? CopyTexCoords(0, first, count) : RepeatTexCoord(0, count);
	else
		v.st0 = 0;
	if (unit_enabled[1])
		v.st1 = array_st[1].enabled ? CopyTexCoords(1, first, count) : RepeatTexCoord(1, count);
	else
		v.st1 = 0;

	DEV_SetVertices(&v);
}

static void APIENTRY dkglDrawArrays (GLenum mode, GLint first, GLsizei count)
{
	if (count <= 0 || !array_pos.pointer)
		return;
	SetupArrays(first, count);
	FlushState();
	DEV_Draw(mode, 0, count);
}

static void APIENTRY dkglDrawElements (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	const unsigned	*in = indices;
	devAddr_t	index_addr;
	int		vertices;

	if (count <= 0 || !array_pos.pointer || type != GL_UNSIGNED_INT)
		return;

	if (lock_count)
	{
		vertices = lock_count;
	}
	else
	{
		unsigned	highest = 0;
		int		i;

		for (i = 0; i < count; i++)
			if (in[i] > highest)
				highest = in[i];
		vertices = highest + 1;
	}

	SetupArrays(0, vertices);

	/* a shader's stages draw the same triangles with different colors and coordinates */
	if (lock_count && lock_indices == indices && lock_indices_count == count)
	{
		index_addr = lock_indices_addr;
	}
	else
	{
		memcpy(DEV_Stream(count * sizeof(unsigned), &index_addr), in, count * sizeof(unsigned));
		if (lock_count)
		{
			lock_indices = indices;
			lock_indices_count = count;
			lock_indices_addr = index_addr;
		}
	}

	FlushState();
	DEV_DrawIndexed(mode, index_addr, count);
}

static void APIENTRY dkglLockArraysEXT (GLint first, GLsizei count)
{
	(void) first;
	lock_count = count;
	lock_pos_addr = 0;
	lock_indices = NULL;
}

static void APIENTRY dkglUnlockArraysEXT (void)
{
	lock_count = 0;
	lock_pos_addr = 0;
	lock_indices = NULL;
}

/*
===============================================================================

IMMEDIATE MODE: skies, flares, debug views and 2D, as short arrays

===============================================================================
*/

static void APIENTRY dkglBegin (GLenum mode)
{
	imm_mode = mode;
	imm_count = 0;
}

static byte ColorByte (float value)
{
	return value <= 0 ? 0 : (value >= 1 ? 255 : (byte) (value * 255));
}

static void APIENTRY dkglColor4f (GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	current_color[0] = r;
	current_color[1] = g;
	current_color[2] = b;
	current_color[3] = a;
	current_color_b[0] = ColorByte(r);
	current_color_b[1] = ColorByte(g);
	current_color_b[2] = ColorByte(b);
	current_color_b[3] = ColorByte(a);
}

static void APIENTRY dkglColor3f (GLfloat r, GLfloat g, GLfloat b)
{
	dkglColor4f(r, g, b, 1);
}

static void APIENTRY dkglColor3fv (const GLfloat *v)
{
	dkglColor4f(v[0], v[1], v[2], 1);
}

static void APIENTRY dkglColor4ub (GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
	dkglColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

static void APIENTRY dkglColor4ubv (const GLubyte *v)
{
	dkglColor4ub(v[0], v[1], v[2], v[3]);
}

static void APIENTRY dkglTexCoord2f (GLfloat s, GLfloat t)
{
	current_st[0][0] = s;
	current_st[0][1] = t;
}

static void APIENTRY dkglTexCoord2fv (const GLfloat *v)
{
	dkglTexCoord2f(v[0], v[1]);
}

static void APIENTRY dkglMultiTexCoord2fARB (GLenum target, GLfloat s, GLfloat t)
{
	const int	unit = target - GL_TEXTURE0_ARB;

	current_st[unit][0] = s;
	current_st[unit][1] = t;
}

static void APIENTRY dkglVertex3f (GLfloat x, GLfloat y, GLfloat z)
{
	if (imm_count >= IMMEDIATE_VERTICES)
		return;
	imm_pos[imm_count][0] = x;
	imm_pos[imm_count][1] = y;
	imm_pos[imm_count][2] = z;
	imm_pos[imm_count][3] = 1;
	memcpy(imm_color[imm_count], current_color_b, 4);
	imm_st[0][imm_count][0] = current_st[0][0];
	imm_st[0][imm_count][1] = current_st[0][1];
	imm_st[1][imm_count][0] = current_st[1][0];
	imm_st[1][imm_count][1] = current_st[1][1];
	imm_count++;
}

static void APIENTRY dkglVertex3fv (const GLfloat *v)
{
	dkglVertex3f(v[0], v[1], v[2]);
}

static void APIENTRY dkglVertex2f (GLfloat x, GLfloat y)
{
	dkglVertex3f(x, y, 0);
}

static void APIENTRY dkglArrayElement (GLint i)
{
	const float	*p = (const float *) (array_pos.pointer + i * array_pos.stride);

	if (array_color.enabled)
	{
		const byte	*c = array_color.pointer + i * array_color.stride;

		memcpy(current_color_b, c, 4);
	}
	if (array_st[0].enabled)
		memcpy(current_st[0], array_st[0].pointer + i * array_st[0].stride, 2 * sizeof(float));
	if (array_st[1].enabled)
		memcpy(current_st[1], array_st[1].pointer + i * array_st[1].stride, 2 * sizeof(float));
	dkglVertex3f(p[0], p[1], p[2]);
}

static void APIENTRY dkglEnd (void)
{
	static const float	no_color[4] = { 0, 0, 0, 0 };
	devVertices_t		v;

	if (imm_count == 0)
		return;

	memcpy(DEV_Stream(imm_count * 4 * sizeof(float), &v.pos), imm_pos, imm_count * 4 * sizeof(float));
	memcpy(DEV_Stream(imm_count * 4, &v.color), imm_color, imm_count * 4);
	memcpy(DEV_Stream(imm_count * 2 * sizeof(float), &v.st0), imm_st[0], imm_count * 2 * sizeof(float));
	memcpy(DEV_Stream(imm_count * 2 * sizeof(float), &v.st1), imm_st[1], imm_count * 2 * sizeof(float));
	SetConstantColor(no_color, 1.0f);
	DEV_SetVertices(&v);
	FlushState();
	DEV_Draw(imm_mode, 0, imm_count);
	imm_count = 0;
}

/*
===============================================================================

THE REST OF THE FRAME

===============================================================================
*/

static void APIENTRY dkglClear (GLbitfield mask)
{
	if (dirty & DIRTY_STATE)
	{
		DEV_SetRenderState(&state);
		dirty &= ~DIRTY_STATE;
	}
	DEV_Clear(mask, clear_color, clear_depth, clear_stencil);
}

static void APIENTRY dkglReadPixels (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels)
{
	byte	*rgba;
	int	i, count;

	(void) type;
	if (format != GL_RGB && format != GL_RGBA)
	{
		memset(pixels, 0, (size_t) width * height);
		return;
	}
	count = width * height;
	if (format == GL_RGBA)
	{
		DEV_ReadScreen(x, y, width, height, pixels);
		return;
	}

	rgba = malloc((size_t) count * 4);
	if (!rgba)
		return;
	DEV_ReadScreen(x, y, width, height, rgba);
	for (i = 0; i < count; i++)
	{
		((byte *) pixels)[i * 3 + 0] = rgba[i * 4 + 0];
		((byte *) pixels)[i * 3 + 1] = rgba[i * 4 + 1];
		((byte *) pixels)[i * 3 + 2] = rgba[i * 4 + 2];
	}
	free(rgba);
}

/* The renderer asks for the depth under a flare with glReadPixels, and that would stop the
 * GPU for each one. This draws an invisible point at the flare instead, where the renderer's
 * own test would still let it show: up to 24 units behind what is drawn. */
qboolean DKGL_FlareVisible (int id, int window_x, int window_y, float eye_z)
{
	const float		*p = backEnd.viewParms.projectionMatrix;
	const float		z = eye_z + 24.0f;
	const devRenderState_t	saved = state;
	const devVertexUniforms_t saved_uniforms = uniforms;
	devVertices_t		v;
	float			*pos;
	qboolean		visible;

	if (z >= 0)
		return qtrue;	/* in the view's face: nothing can be in front of it */

	pos = DEV_Stream(4 * sizeof(float), &v.pos);
	pos[0] = 2.0f * window_x / glConfig.vidWidth - 1.0f;
	pos[1] = 2.0f * window_y / glConfig.vidHeight - 1.0f;
	pos[2] = (p[10] * z + p[14]) / -z;
	pos[3] = 1;
	v.color = v.st0 = v.st1 = 0;
	DEV_SetVertices(&v);

	state.blend = GL_FALSE;
	state.alpha_test = GL_FALSE;
	state.depth_test = GL_TRUE;
	state.depth_write = GL_FALSE;
	state.depth_func = GL_LEQUAL;
	state.depth_near = 0;
	state.depth_far = 1;
	state.cull = GL_FALSE;
	state.polygon_offset = GL_FALSE;
	state.color_mask = 0;
	state.stencil_test = GL_FALSE;
	state.scissor_test = GL_FALSE;
	state.viewport[0] = state.viewport[1] = 0;
	state.viewport[2] = glConfig.vidWidth;
	state.viewport[3] = glConfig.vidHeight;

	memset(&uniforms, 0, sizeof(uniforms));
	uniforms.mvp[0] = uniforms.mvp[5] = uniforms.mvp[10] = uniforms.mvp[15] = 1;

	DEV_SetRenderState(&state);
	DEV_SetVertexUniforms(&uniforms);
	DEV_SetProgram(DEV_FS_COLOR, qfalse);
	visible = DEV_DepthProbe(id);

	state = saved;
	uniforms = saved_uniforms;
	dirty |= DIRTY_STATE | DIRTY_PROGRAM | DIRTY_UNIFORMS;
	return visible;
}

/*
===============================================================================

SETUP

===============================================================================
*/

void DKGL_Init (void)
{
	int	i;

	if (!imm_pos)
	{
		imm_pos = malloc(IMMEDIATE_VERTICES * sizeof(*imm_pos));
		imm_color = malloc(IMMEDIATE_VERTICES * sizeof(*imm_color));
		imm_st[0] = malloc(IMMEDIATE_VERTICES * sizeof(*imm_st[0]));
		imm_st[1] = malloc(IMMEDIATE_VERTICES * sizeof(*imm_st[1]));
	}

	memset(&state, 0, sizeof(state));
	state.blend_src = GL_ONE;
	state.blend_dst = GL_ZERO;
	state.alpha_func = GL_ALWAYS;
	state.depth_write = GL_TRUE;
	state.depth_func = GL_LESS;
	state.depth_far = 1;
	state.cull_face = GL_BACK;
	state.color_mask = 15;
	state.stencil_func = GL_ALWAYS;
	state.stencil_mask = state.stencil_write_mask = ~0u;
	state.stencil_fail = state.stencil_zfail = state.stencil_zpass = GL_KEEP;
	state.viewport[2] = glConfig.vidWidth;
	state.viewport[3] = glConfig.vidHeight;
	state.scissor[2] = glConfig.vidWidth;
	state.scissor[3] = glConfig.vidHeight;

	memset(&uniforms, 0, sizeof(uniforms));
	modelview_top = projection_top = 0;
	matrix_mode = GL_PROJECTION;
	dkglLoadIdentity();
	matrix_mode = GL_MODELVIEW;
	dkglLoadIdentity();

	active_unit = client_unit = 0;
	for (i = 0; i < DEV_MAX_TEXTURE_UNITS; i++)
	{
		unit_enabled[i] = GL_FALSE;
		unit_texture[i] = 0;
		unit_env[i] = GL_MODULATE;
		memset(&array_st[i], 0, sizeof(array_st[i]));
	}
	memset(&array_pos, 0, sizeof(array_pos));
	memset(&array_color, 0, sizeof(array_color));
	clip_enabled = fog_enabled = GL_FALSE;
	lock_count = 0;

	for (i = 0; i < DEV_MAX_TEXTURES; i++)
	{
		FreePending(&textures[i]);
		textures[i].device_id = -1;
		textures[i].sampler.min_filter = GL_LINEAR;
		textures[i].sampler.mag_filter = GL_LINEAR;
		textures[i].sampler.wrap_s = GL_REPEAT;
		textures[i].sampler.wrap_t = GL_REPEAT;
		textures[i].sampler.anisotropy = 1;
	}

	dirty = DIRTY_ALL;

#define ASSIGN(name) qgl##name = dkgl##name
	ASSIGN(AlphaFunc); ASSIGN(ArrayElement); ASSIGN(Begin); ASSIGN(BindTexture); ASSIGN(BlendFunc); ASSIGN(Clear);
	ASSIGN(ClearColor); ASSIGN(ClearDepth); ASSIGN(ClearStencil); ASSIGN(ClipPlane); ASSIGN(Color3f); ASSIGN(Color3fv);
	ASSIGN(Color4f); ASSIGN(Color4ub); ASSIGN(Color4ubv); ASSIGN(ColorMask); ASSIGN(ColorPointer);
	ASSIGN(CopyTexSubImage2D); ASSIGN(CullFace); ASSIGN(DeleteTextures); ASSIGN(DepthFunc); ASSIGN(DepthMask);
	ASSIGN(DepthRange); ASSIGN(Disable); ASSIGN(DisableClientState); ASSIGN(DrawArrays); ASSIGN(DrawBuffer);
	ASSIGN(DrawElements); ASSIGN(Enable); ASSIGN(EnableClientState); ASSIGN(End); ASSIGN(Finish); ASSIGN(Flush);
	ASSIGN(Fogf); ASSIGN(Fogfv); ASSIGN(Fogi); ASSIGN(Frustum); ASSIGN(GenTextures); ASSIGN(GetBooleanv);
	ASSIGN(GetError); ASSIGN(GetIntegerv); ASSIGN(GetString); ASSIGN(Hint); ASSIGN(IsEnabled); ASSIGN(LineWidth);
	ASSIGN(LoadIdentity); ASSIGN(LoadMatrixf); ASSIGN(MatrixMode); ASSIGN(NormalPointer); ASSIGN(Ortho);
	ASSIGN(PixelStorei); ASSIGN(PolygonMode); ASSIGN(PolygonOffset); ASSIGN(PopMatrix); ASSIGN(PushMatrix);
	ASSIGN(ReadPixels); ASSIGN(Scissor); ASSIGN(ShadeModel); ASSIGN(StencilFunc); ASSIGN(StencilMask); ASSIGN(StencilOp);
	ASSIGN(TexCoord2f); ASSIGN(TexCoord2fv); ASSIGN(TexCoordPointer); ASSIGN(TexEnvf); ASSIGN(TexImage2D);
	ASSIGN(TexParameterf); ASSIGN(TexParameteri); ASSIGN(TexSubImage2D); ASSIGN(Translatef); ASSIGN(Vertex2f);
	ASSIGN(Vertex3f); ASSIGN(Vertex3fv); ASSIGN(VertexPointer); ASSIGN(Viewport);
	ASSIGN(ActiveTextureARB); ASSIGN(ClientActiveTextureARB); ASSIGN(MultiTexCoord2fARB);
	ASSIGN(LockArraysEXT); ASSIGN(UnlockArraysEXT);
#undef ASSIGN
}

void DKGL_Shutdown (void)
{
	int	i;

	for (i = 0; i < DEV_MAX_TEXTURES; i++)
		FreePending(&textures[i]);
}
