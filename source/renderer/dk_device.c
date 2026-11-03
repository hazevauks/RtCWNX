/* dk_device.c -- the renderer's output, on deko3D.
 *
 * Clip space has y up and depth -1 to 1, as on OpenGL, so the renderer's matrices
 * and winding work unchanged. The screen's first row is its top, which differs from
 * OpenGL in viewports, scissors and read backs, all converted here.
 *
 * A frame is drawn to an image of its own. Showing it is one more pass, through the
 * game's gamma table, which on a PC is the video card's ramp: the brightness setting
 * works as it does there, right away and without touching what blends in the frame.
 *
 * State only marks what it touches. Flush writes the commands for what really
 * changed, right before the draw that needs it.
 *
 * What deko3D 0.5.0 needs, learned by the ports that run it on hardware:
 *  - Its release library ends the process silently on an error. Test builds link the
 *    debug library, whose callback writes the error to the log.
 *  - Images need FixTileSize.
 *  - A command can write past what it reserved, so each piece of command memory keeps a gap.
 *  - Clears respect the scissor, as OpenGL's do.
 *  - Coarse depth culling (ZCULL) stays off until tested on the console.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <deko3d.h>

#include "renderer/tr_local.h"

#include "dk_device.h"
#include "nx_platform.h"

#define SWAP_IMAGES		3
#define FRAMES_IN_FLIGHT	2

#define KB			1024u
#define MB			(1024u * 1024u)
#define CMD_MEMORY_PER_FRAME	(2 * MB)
#define CMD_MEMORY_OVERFLOW	(2 * MB)
#define CMD_MEMORY_GAP		256
#define UPLOAD_CMD_MEMORY	(1 * MB)
#define UPLOAD_STAGING_SIZE	(16 * MB)
#define STREAM_PER_FRAME	(8 * MB)	/* the campaign's maps stay under 1 MB (docs/status.md) */
#define SHADER_CODE_SIZE	(256 * KB)
#define ZERO_BUFFER_SIZE	(128 * KB)
#define IMAGE_BLOCK_SIZE	(64 * MB)
#define MAX_IMAGE_BLOCKS	48
#define MAX_FREE_RANGES		2048
#define MAX_SAMPLERS		64
#define PROBE_REPORT_SIZE	16

/* Descriptors past the textures' and samplers' */
#define IMAGE_SCENE		DEV_MAX_TEXTURES
#define IMAGE_GAMMA		(DEV_MAX_TEXTURES + 1)
#define NUM_IMAGE_DESCRIPTORS	(DEV_MAX_TEXTURES + 2)
#define SAMPLER_SCENE		MAX_SAMPLERS
#define SAMPLER_GAMMA		(MAX_SAMPLERS + 1)
#define NUM_SAMPLER_DESCRIPTORS	(MAX_SAMPLERS + 2)

#define ALIGN_UP(value, align)	(((value) + (align) - 1) & ~((align) - 1))

enum
{
	DIRTY_RASTERIZER	= 1 << 0,
	DIRTY_COLOR		= 1 << 1,
	DIRTY_COLOR_WRITE	= 1 << 2,
	DIRTY_BLEND		= 1 << 3,
	DIRTY_DEPTH_STENCIL	= 1 << 4,
	DIRTY_STENCIL_VALUES	= 1 << 5,
	DIRTY_ALPHA_REF		= 1 << 6,
	DIRTY_VIEWPORT		= 1 << 7,
	DIRTY_SCISSOR		= 1 << 8,
	DIRTY_DEPTH_BIAS	= 1 << 9,
	DIRTY_PROGRAM		= 1 << 10,
	DIRTY_VERTEX_UNIFORMS	= 1 << 11,
	DIRTY_FRAGMENT_UNIFORMS	= 1 << 12,
	DIRTY_TEXTURE0		= 1 << 13,
	DIRTY_TEXTURE1		= 1 << 14,
	DIRTY_ALL		= (1 << 15) - 1
};

/* Memory for images comes from large blocks: the system allows few of them. */
typedef struct
{
	uint32_t	offset, size;
} freeRange_t;

typedef struct
{
	DkMemBlock	block;
	freeRange_t	*ranges;
	int		num_ranges;
} imageBlock_t;

typedef struct
{
	qboolean	used;
	DkImage		image;
	int		block;
	uint32_t	offset, size;
	unsigned	free_frame;	/* when it can really go, if it is waiting to */
	qboolean	pending_free;
} deviceTexture_t;

static DkDevice		device;
static DkQueue		queue;
static DkCmdBuf		cmd;		/* the frame's commands */
static DkCmdBuf		upload_cmd;	/* transfers, submitted ahead of the frame */
static DkSwapchain	swapchain;
static DkImage		color_images[SWAP_IMAGES];
static DkImage		depth_image;
static DkImage		scene_image;
static DkImage		gamma_image;
static DkImageView	color_views[SWAP_IMAGES];
static DkImageView	depth_view;
static DkImageView	scene_view;
static DkImageView	gamma_view;
static qboolean		frame_open;
static int		frame_index;
static unsigned		frame_count;
static int		screen_width, screen_height;

static DkMemBlock	framebuffer_block;
static DkMemBlock	cmd_block;
static DkMemBlock	upload_cmd_block;
static DkMemBlock	staging_block;
static DkMemBlock	stream_block;
static DkMemBlock	zero_block;
static DkMemBlock	code_block;
static DkMemBlock	descriptor_block;
static DkMemBlock	uniform_block;
static DkMemBlock	probe_block;
static DkMemBlock	gamma_block;

static DkFence		frame_fences[FRAMES_IN_FLIGHT];
static qboolean		frame_fence_used[FRAMES_IN_FLIGHT];
static uint32_t		overflow_used;

static uint32_t		stream_head;

static uint32_t		staging_head;
static DkFence		staging_fence;
static qboolean		staging_fence_used;
static qboolean		uploads_pending;
static int		upload_commands;
static qboolean		descriptors_dirty;

static imageBlock_t	image_blocks[MAX_IMAGE_BLOCKS];
static int		num_image_blocks;
static uint32_t		image_bytes_used;
static deviceTexture_t	device_textures[DEV_MAX_TEXTURES];
static int		num_textures;

static devSampler_t	sampler_keys[MAX_SAMPLERS];
static int		num_samplers;

/* Shaders */
static DkShader		vertex_shader;
static DkShader		fragment_shaders[DEV_FS_COUNT][2];
static DkShader		present_vertex_shader;
static DkShader		present_fragment_shader;

/* What is set, and what the GPU has yet to hear of */
static unsigned			dirty;
static DkRasterizerState	rasterizer;
static DkColorState		color_state;
static DkColorWriteState	color_write;
static DkBlendState		blend_state;
static DkDepthStencilState	depth_stencil;
static DkMultisampleState	multisample;
static devRenderState_t		render_state;
static devVertexUniforms_t	vertex_uniforms;
static float			fog_color[4];
static DkGpuAddr		vertex_ubo, fragment_ubo;
static uint32_t			vertex_ubo_size, fragment_ubo_size;
static devProgram_t		program;
static qboolean			program_clip;
static DkResHandle		texture_handles[DEV_MAX_TEXTURE_UNITS];
static devVertices_t		vertices;

/* Statistics, for r_dkPerf */
static cvar_t	*r_dkPerf;
static unsigned	stat_draws, stat_frames;
static uint32_t	stat_stream_peak;
static int	stat_last_ms;

extern void Sys_Error (const char *error, ...);

/*
===============================================================================

MEMORY

===============================================================================
*/

static DkMemBlock MakeBlock (uint32_t size, uint32_t flags)
{
	DkMemBlockMaker	maker;

	dkMemBlockMakerDefaults(&maker, device, ALIGN_UP(size, DK_MEMBLOCK_ALIGNMENT));
	maker.flags = flags;
	return dkMemBlockCreate(&maker);
}

static qboolean AddImageBlock (void)
{
	imageBlock_t	*b;

	if (num_image_blocks >= MAX_IMAGE_BLOCKS)
		return qfalse;

	b = &image_blocks[num_image_blocks];
	b->block = MakeBlock(IMAGE_BLOCK_SIZE, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
	if (!b->block)
		return qfalse;
	b->ranges = malloc(MAX_FREE_RANGES * sizeof(freeRange_t));
	b->ranges[0].offset = 0;
	b->ranges[0].size = IMAGE_BLOCK_SIZE;
	b->num_ranges = 1;
	num_image_blocks++;
	return qtrue;
}

static qboolean AllocImageMemory (uint32_t size, uint32_t align, int *out_block, uint32_t *out_offset)
{
	int	attempt, b, i;

	if (size > IMAGE_BLOCK_SIZE)
		return qfalse;

	for (attempt = 0; attempt < 2; attempt++)
	{
		for (b = 0; b < num_image_blocks; b++)
		{
			imageBlock_t	*block = &image_blocks[b];

			for (i = 0; i < block->num_ranges; i++)
			{
				freeRange_t	*r = &block->ranges[i];
				const uint32_t	start = ALIGN_UP(r->offset, align);
				const uint32_t	pad = start - r->offset;
				uint32_t	rest;

				if (pad + size > r->size)
					continue;
				rest = r->size - pad - size;

				if (pad && rest && block->num_ranges == MAX_FREE_RANGES)
					continue;
				if (pad)
				{
					r->size = pad;
					if (rest)
					{
						memmove(&block->ranges[i + 2], &block->ranges[i + 1], (block->num_ranges - i - 1) * sizeof(freeRange_t));
						block->ranges[i + 1].offset = start + size;
						block->ranges[i + 1].size = rest;
						block->num_ranges++;
					}
				}
				else if (rest)
				{
					r->offset = start + size;
					r->size = rest;
				}
				else
				{
					memmove(r, r + 1, (block->num_ranges - i - 1) * sizeof(freeRange_t));
					block->num_ranges--;
				}
				*out_block = b;
				*out_offset = start;
				image_bytes_used += size;
				return qtrue;
			}
		}
		if (!AddImageBlock())
			return qfalse;
	}
	return qfalse;
}

static void FreeImageMemory (int b, uint32_t offset, uint32_t size)
{
	imageBlock_t	*block = &image_blocks[b];
	freeRange_t	*ranges = block->ranges;
	int		i = 0;

	image_bytes_used -= size;

	while (i < block->num_ranges && ranges[i].offset < offset)
		i++;
	if (i > 0 && ranges[i - 1].offset + ranges[i - 1].size == offset)
	{
		ranges[i - 1].size += size;
		if (i < block->num_ranges && ranges[i - 1].offset + ranges[i - 1].size == ranges[i].offset)
		{
			ranges[i - 1].size += ranges[i].size;
			memmove(&ranges[i], &ranges[i + 1], (block->num_ranges - i - 1) * sizeof(freeRange_t));
			block->num_ranges--;
		}
	}
	else if (i < block->num_ranges && offset + size == ranges[i].offset)
	{
		ranges[i].offset = offset;
		ranges[i].size += size;
	}
	else if (block->num_ranges < MAX_FREE_RANGES)
	{
		memmove(&ranges[i + 1], &ranges[i], (block->num_ranges - i) * sizeof(freeRange_t));
		ranges[i].offset = offset;
		ranges[i].size = size;
		block->num_ranges++;
	}
}

/*
===============================================================================

COMMAND MEMORY AND UPLOADS

Each frame has its own slice. One that outgrows it takes more from the overflow
area of the same slice, and runs out only if the scene is enormous.

===============================================================================
*/

static uint32_t FrameCmdOffset (int frame)
{
	return frame * (CMD_MEMORY_PER_FRAME + CMD_MEMORY_OVERFLOW);
}

static void CmdOutOfMemory (void *user_data, DkCmdBuf cmdbuf, size_t min_size)
{
	const uint32_t	size = 1 * MB;

	(void) user_data;
	if (overflow_used + size > CMD_MEMORY_OVERFLOW || min_size > size)
		ri.Error(ERR_FATAL, "deko3D: the frame's command memory ran out");
	dkCmdBufAddMemory(cmdbuf, cmd_block, FrameCmdOffset(frame_index) + CMD_MEMORY_PER_FRAME + overflow_used, size - CMD_MEMORY_GAP);
	overflow_used += size;
}

static void ResetFrameCommands (void)
{
	overflow_used = 0;
	dkCmdBufClear(cmd);
	dkCmdBufAddMemory(cmd, cmd_block, FrameCmdOffset(frame_index), CMD_MEMORY_PER_FRAME - CMD_MEMORY_GAP);
}

static void ResetUploadCommands (void)
{
	upload_commands = 0;
	dkCmdBufClear(upload_cmd);
	dkCmdBufAddMemory(upload_cmd, upload_cmd_block, 0, UPLOAD_CMD_MEMORY - CMD_MEMORY_GAP);
}

/* only the debug library reports through this; the release one ends the process silently */
static void DebugError (void *user_data, const char *context, DkResult result, const char *message)
{
	(void) user_data;
	NX_LogPrintf("deko3D error %d in %s: %s\n", result, context ? context : "?", message ? message : "");
	NX_LogFlush();
	if (result != DkResult_Success)
		ri.Error(ERR_FATAL, "deko3D error %d in %s: %s", result, context ? context : "?", message ? message : "");
}

static void CheckQueue (const char *where)
{
	if (dkQueueIsInErrorState(queue))
	{
		NX_LogFlush();
		ri.Error(ERR_FATAL, "deko3D: the GPU reported a fault (%s), after %u frames", where, frame_count);
	}
}

static void SubmitUploads (void)
{
	if (!uploads_pending && !descriptors_dirty)
		return;
	/* new images need their descriptors read again, and the cache of what was overwritten must go */
	dkCmdBufBarrier(upload_cmd, DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
	dkQueueSubmitCommands(queue, dkCmdBufFinishList(upload_cmd));
	dkQueueSignalFence(queue, &staging_fence, false);
	dkQueueFlush(queue);
	staging_fence_used = qtrue;
	uploads_pending = qfalse;
	descriptors_dirty = qfalse;
}

static void *UploadSpace (uint32_t size, uint32_t align, DkGpuAddr *gpu_addr)
{
	/* a copy takes a bit over 100 bytes of command memory: a map's textures, mip by mip, would outgrow it */
	const int	max_commands = (UPLOAD_CMD_MEMORY / 2) / 128;
	uint32_t	start = ALIGN_UP(staging_head, align);

	if (size > UPLOAD_STAGING_SIZE)
		ri.Error(ERR_FATAL, "deko3D: a transfer of %u bytes is too large", size);

	if (start + size > UPLOAD_STAGING_SIZE || ++upload_commands > max_commands)
	{
		/* everything queued must finish before the staging memory is used again */
		SubmitUploads();
		if (staging_fence_used)
		{
			dkFenceWait(&staging_fence, -1);
			staging_fence_used = qfalse;
		}
		ResetUploadCommands();
		start = 0;
	}
	else if (!uploads_pending && staging_fence_used && dkFenceWait(&staging_fence, 0) == DkResult_Success)
	{
		staging_fence_used = qfalse;
		ResetUploadCommands();
		start = 0;
	}
	staging_head = start + size;
	uploads_pending = qtrue;
	if (upload_commands == 0)
		upload_commands = 1;
	*gpu_addr = dkMemBlockGetGpuAddr(staging_block) + start;
	return (byte *) dkMemBlockGetCpuAddr(staging_block) + start;
}

/*
===============================================================================

TEXTURES

===============================================================================
*/

static DkImageDescriptor *ImageDescriptors (void)
{
	return (DkImageDescriptor *) dkMemBlockGetCpuAddr(descriptor_block);
}

static DkSamplerDescriptor *SamplerDescriptors (void)
{
	return (DkSamplerDescriptor *) ((byte *) dkMemBlockGetCpuAddr(descriptor_block)
			+ NUM_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor));
}

/* deko3D 0.5.0 picks an image's tile height from 1.5 times its height, and its layout
 * shrinks it for the first level, but the descriptor and the engines use the unshrunk
 * one. Where they differ, the second column of blocks lands on the next mip and its
 * upload overwrites it. Asking for the shrunk tile makes them agree. */
static void FixTileSize (DkImageLayoutMaker *maker, uint32_t rows)
{
	const uint32_t	gobs = (rows + rows / 2 + 7) / 8;
	const uint32_t	tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
	uint32_t	shrunk = tile;

	while (shrunk && (8u << (shrunk - 1)) >= rows)
		shrunk--;
	if (shrunk != tile)
	{
		maker->flags |= DkImageFlags_CustomTileSize;
		maker->tileSize = (DkTileSize) shrunk;
	}
}

int DEV_TextureCreate (int width, int height, int levels, qboolean has_alpha)
{
	DkImageLayoutMaker	maker;
	DkImageLayout		layout;
	DkImageView		view;
	deviceTexture_t		*t = NULL;
	uint32_t		size, align;
	int			id;

	for (id = 0; id < DEV_MAX_TEXTURES; id++)
	{
		if (!device_textures[id].used)
		{
			t = &device_textures[id];
			break;
		}
	}
	if (!t)
		ri.Error(ERR_FATAL, "deko3D: out of textures");

	dkImageLayoutMakerDefaults(&maker, device);
	maker.format = DkImageFormat_RGBA8_Unorm;
	maker.dimensions[0] = width;
	maker.dimensions[1] = height;
	maker.mipLevels = levels;
	FixTileSize(&maker, height);
	dkImageLayoutInitialize(&layout, &maker);

	size = (uint32_t) dkImageLayoutGetSize(&layout);
	align = dkImageLayoutGetAlignment(&layout);
	if (!AllocImageMemory(size, align, &t->block, &t->offset))
	{
		ri.Error(ERR_FATAL, "deko3D: out of memory for a %dx%d image, with %u MB in use",
				width, height, image_bytes_used / MB);
	}
	t->used = qtrue;
	t->pending_free = qfalse;
	t->size = size;
	dkImageInitialize(&t->image, &layout, image_blocks[t->block].block, t->offset);

	dkImageViewDefaults(&view, &t->image);
	if (!has_alpha)
		view.swizzle[3] = DkImageSwizzle_One;
	dkImageDescriptorInitialize(&ImageDescriptors()[id], &view, false, false);
	descriptors_dirty = qtrue;
	num_textures++;
	return id;
}

void DEV_TextureUpload (int id, int level, int x, int y, int width, int height, const void *rgba)
{
	deviceTexture_t	*t = &device_textures[id];
	const uint32_t	row_bytes = width * 4;
	const uint32_t	size = row_bytes * height;
	DkGpuAddr	source;
	DkImageView	view;
	DkCopyBuf	buffer;
	DkImageRect	rect;
	void		*space;

	space = UploadSpace(size, 16, &source);
	memcpy(space, rgba, size);

	dkImageViewDefaults(&view, &t->image);
	view.mipLevelOffset = level;
	view.mipLevelCount = 1;

	buffer.addr = source;
	buffer.rowLength = row_bytes;
	buffer.imageHeight = size;
	rect.x = x;
	rect.y = y;
	rect.z = 0;
	rect.width = width;
	rect.height = height;
	rect.depth = 1;
	dkCmdBufCopyBufferToImage(upload_cmd, &buffer, &view, &rect, 0);
}

/* Frames that were drawn with it may still be going. */
void DEV_TextureDestroy (int id)
{
	deviceTexture_t	*t = &device_textures[id];

	t->pending_free = qtrue;
	t->free_frame = frame_count;
}

static void RunDeferredFrees (qboolean all)
{
	int	id;

	for (id = 0; id < DEV_MAX_TEXTURES; id++)
	{
		deviceTexture_t	*t = &device_textures[id];

		if (t->used && t->pending_free && (all || frame_count - t->free_frame >= FRAMES_IN_FLIGHT + 1))
		{
			FreeImageMemory(t->block, t->offset, t->size);
			t->used = qfalse;
			t->pending_free = qfalse;
			num_textures--;
		}
	}
}

static DkFilter FilterFor (GLenum filter)
{
	switch (filter)
	{
	case GL_NEAREST:
	case GL_NEAREST_MIPMAP_NEAREST:
	case GL_NEAREST_MIPMAP_LINEAR:
		return DkFilter_Nearest;
	default:
		return DkFilter_Linear;
	}
}

static DkMipFilter MipFilterFor (GLenum filter)
{
	switch (filter)
	{
	case GL_NEAREST_MIPMAP_NEAREST:
	case GL_LINEAR_MIPMAP_NEAREST:
		return DkMipFilter_Nearest;
	case GL_NEAREST_MIPMAP_LINEAR:
	case GL_LINEAR_MIPMAP_LINEAR:
		return DkMipFilter_Linear;
	default:
		return DkMipFilter_None;
	}
}

static DkWrapMode WrapFor (GLenum wrap)
{
	return wrap == GL_REPEAT ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
}

static int SamplerIndex (const devSampler_t *key)
{
	DkSampler	sampler;
	int		i;

	for (i = 0; i < num_samplers; i++)
	{
		const devSampler_t	*k = &sampler_keys[i];

		if (k->min_filter == key->min_filter && k->mag_filter == key->mag_filter
				&& k->wrap_s == key->wrap_s && k->wrap_t == key->wrap_t && k->anisotropy == key->anisotropy)
			return i;
	}
	if (num_samplers == MAX_SAMPLERS)
		return 0;

	dkSamplerDefaults(&sampler);
	sampler.minFilter = FilterFor(key->min_filter);
	sampler.magFilter = FilterFor(key->mag_filter);
	sampler.mipFilter = MipFilterFor(key->min_filter);
	sampler.wrapMode[0] = WrapFor(key->wrap_s);
	sampler.wrapMode[1] = WrapFor(key->wrap_t);
	sampler.wrapMode[2] = DkWrapMode_Repeat;
	if (key->anisotropy > 1)
		sampler.maxAnisotropy = (float) key->anisotropy;
	dkSamplerDescriptorInitialize(&SamplerDescriptors()[num_samplers], &sampler);
	sampler_keys[num_samplers] = *key;
	descriptors_dirty = qtrue;
	return num_samplers++;
}

void DEV_SetTexture (int unit, int id, const devSampler_t *sampler)
{
	const DkResHandle	handle = dkMakeTextureHandle(id, SamplerIndex(sampler));

	if (texture_handles[unit] != handle)
	{
		texture_handles[unit] = handle;
		dirty |= DIRTY_TEXTURE0 << unit;
	}
}

/*
===============================================================================

STATE

===============================================================================
*/

static DkBlendFactor BlendFactorFor (GLenum factor)
{
	switch (factor)
	{
	case GL_ZERO:			return DkBlendFactor_Zero;
	case GL_ONE:			return DkBlendFactor_One;
	case GL_SRC_COLOR:		return DkBlendFactor_SrcColor;
	case GL_ONE_MINUS_SRC_COLOR:	return DkBlendFactor_InvSrcColor;
	case GL_DST_COLOR:		return DkBlendFactor_DstColor;
	case GL_ONE_MINUS_DST_COLOR:	return DkBlendFactor_InvDstColor;
	case GL_SRC_ALPHA:		return DkBlendFactor_SrcAlpha;
	case GL_ONE_MINUS_SRC_ALPHA:	return DkBlendFactor_InvSrcAlpha;
	case GL_DST_ALPHA:		return DkBlendFactor_DstAlpha;
	case GL_ONE_MINUS_DST_ALPHA:	return DkBlendFactor_InvDstAlpha;
	case GL_SRC_ALPHA_SATURATE:	return DkBlendFactor_SrcAlphaSaturate;
	default:			return DkBlendFactor_One;
	}
}

static DkCompareOp CompareFor (GLenum func)
{
	switch (func)
	{
	case GL_NEVER:		return DkCompareOp_Never;
	case GL_LESS:		return DkCompareOp_Less;
	case GL_EQUAL:		return DkCompareOp_Equal;
	case GL_LEQUAL:		return DkCompareOp_Lequal;
	case GL_GREATER:	return DkCompareOp_Greater;
	case GL_NOTEQUAL:	return DkCompareOp_NotEqual;
	case GL_GEQUAL:		return DkCompareOp_Gequal;
	default:		return DkCompareOp_Always;
	}
}

static DkStencilOp StencilOpFor (GLenum op)
{
	switch (op)
	{
	case GL_ZERO:		return DkStencilOp_Zero;
	case GL_REPLACE:	return DkStencilOp_Replace;
	case GL_INCR:		return DkStencilOp_Incr;
	case GL_DECR:		return DkStencilOp_Decr;
	case GL_INVERT:		return DkStencilOp_Invert;
	default:		return DkStencilOp_Keep;
	}
}

void DEV_SetRenderState (const devRenderState_t *s)
{
	const DkFace		cull = !s->cull ? DkFace_None : (s->cull_face == GL_FRONT ? DkFace_Front
			: (s->cull_face == GL_BACK ? DkFace_Back : DkFace_FrontAndBack));
	const DkPolygonMode	polygon = s->polygon_line ? DkPolygonMode_Line : DkPolygonMode_Fill;
	const uint32_t		bias_mask = s->polygon_offset ? DkPolygonFlag_All : 0;
	const bool		depth_write = s->depth_test && s->depth_write;

	if (rasterizer.cullMode != cull || rasterizer.polygonModeFront != polygon || rasterizer.depthBiasEnableMask != bias_mask)
	{
		rasterizer.cullMode = cull;
		rasterizer.polygonModeFront = polygon;
		rasterizer.polygonModeBack = polygon;
		rasterizer.depthBiasEnableMask = bias_mask;
		dirty |= DIRTY_RASTERIZER;
	}

	if (render_state.blend != s->blend || render_state.blend_src != s->blend_src || render_state.blend_dst != s->blend_dst)
	{
		const DkBlendFactor	src = BlendFactorFor(s->blend_src);
		const DkBlendFactor	dst = BlendFactorFor(s->blend_dst);

		dkBlendStateSetFactors(&blend_state, src, dst, src, dst);
		dkColorStateSetBlendEnable(&color_state, 0, s->blend);
		dirty |= DIRTY_BLEND | DIRTY_COLOR;
	}

	if (render_state.alpha_test != s->alpha_test || render_state.alpha_func != s->alpha_func)
	{
		color_state.alphaCompareOp = s->alpha_test ? CompareFor(s->alpha_func) : DkCompareOp_Always;
		dirty |= DIRTY_COLOR;
	}
	if (render_state.alpha_ref != s->alpha_ref)
		dirty |= DIRTY_ALPHA_REF;

	if (render_state.color_mask != s->color_mask)
	{
		dkColorWriteStateSetMask(&color_write, 0, s->color_mask);
		dirty |= DIRTY_COLOR_WRITE;
	}

	if (depth_stencil.depthTestEnable != (bool) s->depth_test || depth_stencil.depthWriteEnable != depth_write
			|| render_state.depth_func != s->depth_func
			|| depth_stencil.stencilTestEnable != (bool) s->stencil_test || render_state.stencil_func != s->stencil_func
			|| render_state.stencil_fail != s->stencil_fail || render_state.stencil_zfail != s->stencil_zfail
			|| render_state.stencil_zpass != s->stencil_zpass)
	{
		depth_stencil.depthTestEnable = s->depth_test;
		depth_stencil.depthWriteEnable = depth_write;
		depth_stencil.depthCompareOp = CompareFor(s->depth_func);
		depth_stencil.stencilTestEnable = s->stencil_test;
		depth_stencil.stencilFrontCompareOp = depth_stencil.stencilBackCompareOp = CompareFor(s->stencil_func);
		depth_stencil.stencilFrontFailOp = depth_stencil.stencilBackFailOp = StencilOpFor(s->stencil_fail);
		depth_stencil.stencilFrontDepthFailOp = depth_stencil.stencilBackDepthFailOp = StencilOpFor(s->stencil_zfail);
		depth_stencil.stencilFrontPassOp = depth_stencil.stencilBackPassOp = StencilOpFor(s->stencil_zpass);
		dirty |= DIRTY_DEPTH_STENCIL;
	}

	if (render_state.stencil_ref != s->stencil_ref || render_state.stencil_mask != s->stencil_mask
			|| render_state.stencil_write_mask != s->stencil_write_mask)
		dirty |= DIRTY_STENCIL_VALUES;

	if (memcmp(render_state.viewport, s->viewport, sizeof(s->viewport))
			|| render_state.depth_near != s->depth_near || render_state.depth_far != s->depth_far)
		dirty |= DIRTY_VIEWPORT;

	if (render_state.scissor_test != s->scissor_test || memcmp(render_state.scissor, s->scissor, sizeof(s->scissor)))
		dirty |= DIRTY_SCISSOR;

	if (render_state.offset_factor != s->offset_factor || render_state.offset_units != s->offset_units)
		dirty |= DIRTY_DEPTH_BIAS;

	render_state = *s;
}

void DEV_SetProgram (devProgram_t new_program, qboolean clip)
{
	if (program != new_program || program_clip != clip)
	{
		program = new_program;
		program_clip = clip;
		dirty |= DIRTY_PROGRAM;
	}
}

void DEV_SetVertexUniforms (const devVertexUniforms_t *uniforms)
{
	vertex_uniforms = *uniforms;
	dirty |= DIRTY_VERTEX_UNIFORMS;
}

void DEV_SetFogColor (const float *rgba)
{
	memcpy(fog_color, rgba, sizeof(fog_color));
	dirty |= DIRTY_FRAGMENT_UNIFORMS;
}

/*
===============================================================================

FRAMES AND DRAWS

===============================================================================
*/

static void BeginFrame (void)
{
	static const DkVtxAttribState attribs[] =
	{
		{ 0, 0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
		{ 1, 0, 0, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0 },
		{ 2, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
		{ 3, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
	};
	static const DkVtxBufferState buffers[] =
	{
		{ 4 * sizeof(float), 0 },
		{ 4, 0 },
		{ 2 * sizeof(float), 0 },
		{ 2 * sizeof(float), 0 },
	};
	const DkImageView	*color = &scene_view;

	if (frame_open)
		return;
	frame_open = qtrue;

	SubmitUploads();
	CheckQueue("begin");

	dkCmdBufBindRenderTargets(cmd, &color, 1, &depth_view);
	dkCmdBufBindImageDescriptorSet(cmd, dkMemBlockGetGpuAddr(descriptor_block), NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(cmd, dkMemBlockGetGpuAddr(descriptor_block) + NUM_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor), NUM_SAMPLER_DESCRIPTORS);
	dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader);
	dkCmdBufBindMultisampleState(cmd, &multisample);
	dkCmdBufBindVtxAttribState(cmd, attribs, 4);
	dkCmdBufBindVtxBufferState(cmd, buffers, 4);
	dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, vertex_ubo, vertex_ubo_size);
	dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, fragment_ubo, fragment_ubo_size);

	dirty = DIRTY_ALL;
}

/* The engine measures y from the bottom of the screen. */
static void Flush (void)
{
	const unsigned	d = dirty;

	BeginFrame();
	if (!dirty)
		return;
	dirty = 0;

	if (d & DIRTY_PROGRAM)
	{
		const DkShader	*shaders[2] = { &vertex_shader, &fragment_shaders[program][program_clip ? 1 : 0] };

		dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
	}
	if (d & DIRTY_RASTERIZER)
		dkCmdBufBindRasterizerState(cmd, &rasterizer);
	if (d & DIRTY_COLOR)
		dkCmdBufBindColorState(cmd, &color_state);
	if (d & DIRTY_COLOR_WRITE)
		dkCmdBufBindColorWriteState(cmd, &color_write);
	if (d & DIRTY_BLEND)
		dkCmdBufBindBlendState(cmd, 0, &blend_state);
	if (d & DIRTY_DEPTH_STENCIL)
		dkCmdBufBindDepthStencilState(cmd, &depth_stencil);
	if (d & DIRTY_STENCIL_VALUES)
	{
		dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, (uint8_t) render_state.stencil_write_mask,
				(uint8_t) render_state.stencil_ref, (uint8_t) render_state.stencil_mask);
	}
	if (d & DIRTY_ALPHA_REF)
		dkCmdBufSetAlphaRef(cmd, render_state.alpha_ref);
	if (d & DIRTY_VIEWPORT)
	{
		const int	*v = render_state.viewport;
		DkViewport	viewport =
		{
			(float) v[0], (float) (screen_height - v[1] - v[3]), (float) v[2], (float) v[3],
			render_state.depth_near, render_state.depth_far
		};

		dkCmdBufSetViewports(cmd, 0, &viewport, 1);
	}
	if (d & DIRTY_SCISSOR)
	{
		DkScissor	scissor = { 0, 0, (uint32_t) screen_width, (uint32_t) screen_height };

		if (render_state.scissor_test)
		{
			const int	*s = render_state.scissor;
			const int	top = screen_height - s[1] - s[3];

			scissor.x = s[0] < 0 ? 0 : s[0];
			scissor.y = top < 0 ? 0 : top;
			scissor.width = s[2] < 0 ? 0 : s[2];
			scissor.height = s[3] < 0 ? 0 : s[3];
		}
		dkCmdBufSetScissors(cmd, 0, &scissor, 1);
	}
	if (d & DIRTY_DEPTH_BIAS)
		dkCmdBufSetDepthBias(cmd, render_state.offset_units, 0.0f, render_state.offset_factor);
	if (d & DIRTY_VERTEX_UNIFORMS)
		dkCmdBufPushConstants(cmd, vertex_ubo, vertex_ubo_size, 0, sizeof(vertex_uniforms), &vertex_uniforms);
	if (d & DIRTY_FRAGMENT_UNIFORMS)
		dkCmdBufPushConstants(cmd, fragment_ubo, fragment_ubo_size, 0, sizeof(fog_color), fog_color);
	if (d & DIRTY_TEXTURE0)
		dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, texture_handles[0]);
	if (d & DIRTY_TEXTURE1)
		dkCmdBufBindTexture(cmd, DkStage_Fragment, 1, texture_handles[1]);
}

void *DEV_Stream (uint32_t size, devAddr_t *addr)
{
	uint32_t	start = ALIGN_UP(stream_head, 16u);

	if (start + size > STREAM_PER_FRAME)
	{
		/* More than a frame should ever draw: let the GPU catch up, and start the slice over. */
		NX_LogPrintf("WARNING: deko3D: the frame's %u MB of vertices ran out\n", STREAM_PER_FRAME / MB);
		BeginFrame();
		dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmd));
		dkQueueWaitIdle(queue);
		start = 0;
	}
	stream_head = start + size;
	*addr = dkMemBlockGetGpuAddr(stream_block) + frame_index * STREAM_PER_FRAME + start;
	return (byte *) dkMemBlockGetCpuAddr(stream_block) + frame_index * STREAM_PER_FRAME + start;
}

void DEV_SetVertices (const devVertices_t *v)
{
	vertices = *v;
}

static DkPrimitive PrimitiveFor (GLenum mode)
{
	switch (mode)
	{
	case GL_POINTS:		return DkPrimitive_Points;
	case GL_LINES:		return DkPrimitive_Lines;
	case GL_LINE_LOOP:	return DkPrimitive_LineLoop;
	case GL_LINE_STRIP:	return DkPrimitive_LineStrip;
	case GL_TRIANGLE_STRIP:	return DkPrimitive_TriangleStrip;
	case GL_TRIANGLE_FAN:	return DkPrimitive_TriangleFan;
	case GL_QUADS:		return DkPrimitive_Quads;
	case GL_QUAD_STRIP:	return DkPrimitive_QuadStrip;
	case GL_POLYGON:	return DkPrimitive_Polygon;
	default:		return DkPrimitive_Triangles;
	}
}

static void BindVertices (void)
{
	const DkGpuAddr	zero = dkMemBlockGetGpuAddr(zero_block);

	dkCmdBufBindVtxBuffer(cmd, 0, vertices.pos, STREAM_PER_FRAME);
	dkCmdBufBindVtxBuffer(cmd, 1, vertices.color ? vertices.color : zero, vertices.color ? STREAM_PER_FRAME : ZERO_BUFFER_SIZE);
	dkCmdBufBindVtxBuffer(cmd, 2, vertices.st0 ? vertices.st0 : zero, vertices.st0 ? STREAM_PER_FRAME : ZERO_BUFFER_SIZE);
	dkCmdBufBindVtxBuffer(cmd, 3, vertices.st1 ? vertices.st1 : zero, vertices.st1 ? STREAM_PER_FRAME : ZERO_BUFFER_SIZE);
}

void DEV_Draw (GLenum mode, int first, int count)
{
	Flush();
	BindVertices();
	dkCmdBufDraw(cmd, PrimitiveFor(mode), count, 1, first, 0);
	stat_draws++;
}

void DEV_DrawIndexed (GLenum mode, devAddr_t indices, int count)
{
	Flush();
	BindVertices();
	dkCmdBufBindIdxBuffer(cmd, DkIdxFormat_Uint32, indices);
	dkCmdBufDrawIndexed(cmd, PrimitiveFor(mode), count, 1, 0, 0, 0);
	stat_draws++;
}

void DEV_Clear (GLbitfield mask, const float *color, float depth, int stencil)
{
	Flush();
	if (mask & GL_COLOR_BUFFER_BIT)
		dkCmdBufClearColorFloat(cmd, 0, render_state.color_mask, color[0], color[1], color[2], color[3]);
	if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))
	{
		dkCmdBufClearDepthStencil(cmd, (mask & GL_DEPTH_BUFFER_BIT) != 0, depth,
				(mask & GL_STENCIL_BUFFER_BIT) ? 0xff : 0, (uint8_t) stencil);
	}
}

static void LogPerformance (void)
{
	const int	now = ri.Milliseconds();

	if (stream_head > stat_stream_peak)
		stat_stream_peak = stream_head;
	stat_frames++;

	if (!r_dkPerf->integer)
		return;
	if (now - stat_last_ms >= 1000)
	{
		NX_LogPrintf("perf: %.1f fps, %u draws a frame, %u KB of vertices at most, %u images in %u MB\n",
				stat_frames * 1000.0f / (now - stat_last_ms), stat_draws / (stat_frames ? stat_frames : 1),
				stat_stream_peak / KB, num_textures, image_bytes_used / MB);
		stat_last_ms = now;
		stat_frames = 0;
		stat_draws = 0;
		stat_stream_peak = 0;
	}
}

void DEV_SetGammaTable (const unsigned char *red, const unsigned char *green, const unsigned char *blue)
{
	DkGpuAddr	source;
	byte		*space = UploadSpace(256 * 4, 16, &source);
	DkCopyBuf	buffer = { source, 256 * 4, 256 * 4 };
	DkImageRect	rect = { 0, 0, 0, 256, 1, 1 };
	int		i;

	for (i = 0; i < 256; i++)
	{
		space[i * 4 + 0] = red[i];
		space[i * 4 + 1] = green[i];
		space[i * 4 + 2] = blue[i];
		space[i * 4 + 3] = 255;
	}
	dkCmdBufCopyBufferToImage(upload_cmd, &buffer, &gamma_view, &rect, 0);
}

/* The frame, through the gamma table, onto the image the system shows. */
static void DrawToScreen (int slot)
{
	const DkImageView	*color = &color_views[slot];
	const DkShader		*shaders[2] = { &present_vertex_shader, &present_fragment_shader };
	DkViewport		viewport = { 0, 0, (float) screen_width, (float) screen_height, 0, 1 };
	DkScissor		scissor = { 0, 0, (uint32_t) screen_width, (uint32_t) screen_height };
	DkRasterizerState	plain_rasterizer;
	DkColorState		plain_color;
	DkColorWriteState	plain_write;
	DkDepthStencilState	plain_depth;

	dkRasterizerStateDefaults(&plain_rasterizer);
	plain_rasterizer.cullMode = DkFace_None;
	dkColorStateDefaults(&plain_color);
	dkColorWriteStateDefaults(&plain_write);
	dkDepthStencilStateDefaults(&plain_depth);
	plain_depth.depthTestEnable = false;
	plain_depth.depthWriteEnable = false;

	dkCmdBufDiscardDepthStencil(cmd);
	dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
	dkCmdBufBindRenderTargets(cmd, &color, 1, NULL);
	dkCmdBufSetViewports(cmd, 0, &viewport, 1);
	dkCmdBufSetScissors(cmd, 0, &scissor, 1);
	dkCmdBufBindRasterizerState(cmd, &plain_rasterizer);
	dkCmdBufBindColorState(cmd, &plain_color);
	dkCmdBufBindColorWriteState(cmd, &plain_write);
	dkCmdBufBindDepthStencilState(cmd, &plain_depth);
	dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
	dkCmdBufBindVtxAttribState(cmd, NULL, 0);
	dkCmdBufBindVtxBufferState(cmd, NULL, 0);
	dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(IMAGE_SCENE, SAMPLER_SCENE));
	dkCmdBufBindTexture(cmd, DkStage_Fragment, 1, dkMakeTextureHandle(IMAGE_GAMMA, SAMPLER_GAMMA));
	dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
}

void DEV_Present (void)
{
	int	slot;

	BeginFrame();
	SubmitUploads();
	CheckQueue("present");
	LogPerformance();

	/* the wait for a free screen is here, after all of the frame's own work */
	slot = dkQueueAcquireImage(queue, swapchain);
	DrawToScreen(slot);

	dkCmdBufSignalFence(cmd, &frame_fences[frame_index], true);
	dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmd));
	dkQueuePresentImage(queue, swapchain, slot);
	frame_fence_used[frame_index] = qtrue;
	frame_open = qfalse;
	frame_count++;

	/* this slice of command and vertex memory is free once its last frame is done */
	frame_index = (frame_index + 1) % FRAMES_IN_FLIGHT;
	if (frame_fence_used[frame_index])
		dkFenceWait(&frame_fences[frame_index], -1);
	stream_head = 0;
	RunDeferredFrees(qfalse);
	ResetFrameCommands();
}

/*
===============================================================================

READING THE GPU'S WORK

===============================================================================
*/

void DEV_ReadScreen (int x, int y, int width, int height, void *rgba)
{
	static DkMemBlock	capture_block;
	static DkImage		capture_image;
	const uint32_t		size = (uint32_t) width * height * 4;
	DkImageLayoutMaker	maker;
	DkImageLayout		layout;
	DkImageView		capture_view;
	DkImageRect		src, dst;
	DkCopyBuf		buffer;
	DkGpuAddr		staging;
	void			*space;
	const DkImageView	*color;

	Flush();

	/* The screen is compressed for the GPU's eyes only: the 2D engine copies it to plain
	 * memory first. */
	dkImageLayoutMakerDefaults(&maker, device);
	maker.flags = DkImageFlags_Usage2DEngine;
	maker.format = DkImageFormat_RGBA8_Unorm;
	maker.dimensions[0] = screen_width;
	maker.dimensions[1] = screen_height;
	dkImageLayoutInitialize(&layout, &maker);
	if (!capture_block)
	{
		capture_block = MakeBlock((uint32_t) dkImageLayoutGetSize(&layout), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
		if (!capture_block)
			return;
	}
	dkImageInitialize(&capture_image, &layout, capture_block, 0);
	dkImageViewDefaults(&capture_view, &capture_image);

	src.x = x;
	src.y = screen_height - y - height;
	src.z = 0;
	src.width = width;
	src.height = height;
	src.depth = 1;
	dst = src;
	dst.x = 0;
	dst.y = 0;

	dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
	dkCmdBufBlitImage(cmd, &scene_view, &src, &capture_view, &dst, DkBlitFlag_FlipY, 0);
	dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);

	/* whatever is in the staging memory has to go first */
	SubmitUploads();
	space = UploadSpace(size, 16, &staging);
	uploads_pending = qfalse;
	buffer.addr = staging;
	buffer.rowLength = width * 4;
	buffer.imageHeight = size;
	dkCmdBufCopyImageToBuffer(cmd, &capture_view, &dst, &buffer, 0);

	dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmd));
	dkQueueWaitIdle(queue);
	CheckQueue("read back");
	memcpy(rgba, space, size);

	color = &scene_view;
	dkCmdBufBindRenderTargets(cmd, &color, 1, &depth_view);
}

/* Each frame has its reports, written in order by the GPU: the count of samples that
 * passed before the point and after it. The slice about to be used is the oldest, and
 * Present waited for it: that is the answer this asks for. */
qboolean DEV_DepthProbe (int id)
{
	const uint32_t	offset = (frame_index * DEV_MAX_DEPTH_PROBES + id) * 2 * PROBE_REPORT_SIZE;
	const uint64_t	*reports = (const uint64_t *) ((const byte *) dkMemBlockGetCpuAddr(probe_block) + offset);
	const DkGpuAddr	addr = dkMemBlockGetGpuAddr(probe_block) + offset;
	const qboolean	visible = reports[2] > reports[0];

	Flush();
	BindVertices();
	dkCmdBufReportCounter(cmd, DkCounter_SamplesPassed, addr);
	dkCmdBufDraw(cmd, DkPrimitive_Points, 1, 1, 0, 0);
	dkCmdBufReportCounter(cmd, DkCounter_SamplesPassed, addr + PROBE_REPORT_SIZE);
	return visible;
}

/*
===============================================================================

STARTING AND STOPPING

===============================================================================
*/

static void CreateFramebuffers (void)
{
	DkImageLayoutMaker	maker;
	DkImageLayout		depth_layout, color_layout, gamma_layout;
	DkSampler		plain_sampler;
	DkSwapchainMaker	swap_maker;
	const DkImage		*swap_images[SWAP_IMAGES];
	uint32_t		depth_size, color_size, color_align, offset_color;
	int			i;

	dkImageLayoutMakerDefaults(&maker, device);
	maker.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
	maker.format = DkImageFormat_Z24S8;
	maker.dimensions[0] = screen_width;
	maker.dimensions[1] = screen_height;
	dkImageLayoutInitialize(&depth_layout, &maker);

	maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression;
	maker.format = DkImageFormat_RGBA8_Unorm;
	dkImageLayoutInitialize(&color_layout, &maker);

	depth_size = ALIGN_UP((uint32_t) dkImageLayoutGetSize(&depth_layout), dkImageLayoutGetAlignment(&depth_layout));
	color_align = dkImageLayoutGetAlignment(&color_layout);
	color_size = ALIGN_UP((uint32_t) dkImageLayoutGetSize(&color_layout), color_align);
	offset_color = ALIGN_UP(depth_size, color_align);

	/* the frame's own image is laid out as the screen's, one more of them */
	framebuffer_block = MakeBlock(offset_color + color_size * (SWAP_IMAGES + 1), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);

	dkImageInitialize(&depth_image, &depth_layout, framebuffer_block, 0);
	dkImageViewDefaults(&depth_view, &depth_image);

	dkImageInitialize(&scene_image, &color_layout, framebuffer_block, offset_color + SWAP_IMAGES * color_size);
	dkImageViewDefaults(&scene_view, &scene_image);
	dkImageDescriptorInitialize(&ImageDescriptors()[IMAGE_SCENE], &scene_view, false, false);

	/* the gamma table, as a row of pixels */
	dkImageLayoutMakerDefaults(&maker, device);
	maker.format = DkImageFormat_RGBA8_Unorm;
	maker.dimensions[0] = 256;
	maker.dimensions[1] = 1;
	dkImageLayoutInitialize(&gamma_layout, &maker);
	gamma_block = MakeBlock((uint32_t) dkImageLayoutGetSize(&gamma_layout), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
	dkImageInitialize(&gamma_image, &gamma_layout, gamma_block, 0);
	dkImageViewDefaults(&gamma_view, &gamma_image);
	dkImageDescriptorInitialize(&ImageDescriptors()[IMAGE_GAMMA], &gamma_view, false, false);

	dkSamplerDefaults(&plain_sampler);
	plain_sampler.minFilter = plain_sampler.magFilter = DkFilter_Nearest;
	plain_sampler.wrapMode[0] = plain_sampler.wrapMode[1] = plain_sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
	dkSamplerDescriptorInitialize(&SamplerDescriptors()[SAMPLER_SCENE], &plain_sampler);
	plain_sampler.minFilter = plain_sampler.magFilter = DkFilter_Linear;
	dkSamplerDescriptorInitialize(&SamplerDescriptors()[SAMPLER_GAMMA], &plain_sampler);

	for (i = 0; i < SWAP_IMAGES; i++)
	{
		dkImageInitialize(&color_images[i], &color_layout, framebuffer_block, offset_color + i * color_size);
		dkImageViewDefaults(&color_views[i], &color_images[i]);
		swap_images[i] = &color_images[i];
	}

	dkSwapchainMakerDefaults(&swap_maker, device, nwindowGetDefault(), swap_images, SWAP_IMAGES);
	swapchain = dkSwapchainCreate(&swap_maker);
}

/* The renderer's programs, compiled by uam into the NRO's romfs. */
static qboolean LoadShaderFile (const char *name, DkShader *shader, uint32_t *code_head)
{
	char		path[128];
	FILE		*f;
	uint32_t	size, offset;
	DkShaderMaker	maker;
	qboolean	ok;

	snprintf(path, sizeof(path), "romfs:/shaders/%s.dksh", name);
	f = fopen(path, "rb");
	if (!f)
		return qfalse;
	fseek(f, 0, SEEK_END);
	size = (uint32_t) ftell(f);
	fseek(f, 0, SEEK_SET);

	offset = ALIGN_UP(*code_head, DK_SHADER_CODE_ALIGNMENT);
	if (offset + size + DK_SHADER_CODE_UNUSABLE_SIZE > SHADER_CODE_SIZE)
	{
		fclose(f);
		return qfalse;
	}
	ok = fread((byte *) dkMemBlockGetCpuAddr(code_block) + offset, 1, size, f) == size;
	fclose(f);
	if (!ok)
		return qfalse;
	*code_head = offset + size;

	dkShaderMakerDefaults(&maker, code_block, offset);
	dkShaderInitialize(shader, &maker);
	return dkShaderIsValid(shader) ? qtrue : qfalse;
}

static void LoadShaders (void)
{
	static const char	*names[DEV_FS_COUNT] =
	{
		"color", "tex", "tex_replace", "multi_modulate", "multi_add", "multi_decal"
	};
	uint32_t	code_head = 0;
	int		i;

	if (!LoadShaderFile("rtcw_vert", &vertex_shader, &code_head))
		ri.Error(ERR_FATAL, "deko3D: missing shader rtcw_vert");
	if (!LoadShaderFile("present_vert", &present_vertex_shader, &code_head)
			|| !LoadShaderFile("present_frag", &present_fragment_shader, &code_head))
		ri.Error(ERR_FATAL, "deko3D: missing the present shaders");
	for (i = 0; i < DEV_FS_COUNT; i++)
	{
		char	name[64];

		snprintf(name, sizeof(name), "%s_frag", names[i]);
		if (!LoadShaderFile(name, &fragment_shaders[i][0], &code_head))
			ri.Error(ERR_FATAL, "deko3D: missing shader %s", name);
		snprintf(name, sizeof(name), "%s_clip_frag", names[i]);
		if (!LoadShaderFile(name, &fragment_shaders[i][1], &code_head))
			ri.Error(ERR_FATAL, "deko3D: missing shader %s", name);
	}
}

qboolean DEV_Init (int *width, int *height)
{
	DkDeviceMaker	device_maker;
	DkQueueMaker	queue_maker;
	DkCmdBufMaker	cmd_maker;
	const uint32_t	uncached = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	uint32_t	offset;
	int		i;

	r_dkPerf = ri.Cvar_Get("r_dkPerf", "0", 0);
	screen_width = 1280;
	screen_height = 720;

	dkDeviceMakerDefaults(&device_maker);
	device_maker.flags = DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginUpperLeft;
	device_maker.cbDebug = DebugError;
	device = dkDeviceCreate(&device_maker);
	if (!device)
		return qfalse;

	cmd_block = MakeBlock(FRAMES_IN_FLIGHT * (CMD_MEMORY_PER_FRAME + CMD_MEMORY_OVERFLOW), uncached);
	upload_cmd_block = MakeBlock(UPLOAD_CMD_MEMORY, uncached);
	staging_block = MakeBlock(UPLOAD_STAGING_SIZE, uncached);
	stream_block = MakeBlock(FRAMES_IN_FLIGHT * STREAM_PER_FRAME, uncached);
	zero_block = MakeBlock(ZERO_BUFFER_SIZE, uncached);
	code_block = MakeBlock(SHADER_CODE_SIZE, uncached | DkMemBlockFlags_Code);
	descriptor_block = MakeBlock(NUM_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor) + NUM_SAMPLER_DESCRIPTORS * sizeof(DkSamplerDescriptor), uncached);
	uniform_block = MakeBlock(2 * DK_UNIFORM_BUF_ALIGNMENT + sizeof(devVertexUniforms_t) + 16, uncached);
	probe_block = MakeBlock(FRAMES_IN_FLIGHT * DEV_MAX_DEPTH_PROBES * 2 * PROBE_REPORT_SIZE, uncached);

	memset(dkMemBlockGetCpuAddr(zero_block), 0, ZERO_BUFFER_SIZE);
	memset(dkMemBlockGetCpuAddr(probe_block), 0, FRAMES_IN_FLIGHT * DEV_MAX_DEPTH_PROBES * 2 * PROBE_REPORT_SIZE);

	vertex_ubo_size = ALIGN_UP((uint32_t) sizeof(devVertexUniforms_t), DK_UNIFORM_BUF_ALIGNMENT);
	fragment_ubo_size = DK_UNIFORM_BUF_ALIGNMENT;
	offset = 0;
	vertex_ubo = dkMemBlockGetGpuAddr(uniform_block) + offset;
	offset += vertex_ubo_size;
	fragment_ubo = dkMemBlockGetGpuAddr(uniform_block) + offset;

	dkQueueMakerDefaults(&queue_maker, device);
	queue_maker.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio | DkQueueFlags_DisableZcull;
	queue_maker.commandMemorySize = 1 * MB;
	queue_maker.flushThreshold = queue_maker.commandMemorySize / 8;
	queue = dkQueueCreate(&queue_maker);

	dkCmdBufMakerDefaults(&cmd_maker, device);
	cmd_maker.cbAddMem = CmdOutOfMemory;
	cmd = dkCmdBufCreate(&cmd_maker);
	dkCmdBufMakerDefaults(&cmd_maker, device);
	upload_cmd = dkCmdBufCreate(&cmd_maker);

	frame_index = 0;
	frame_count = 0;
	frame_open = qfalse;
	stream_head = 0;
	staging_head = 0;
	staging_fence_used = qfalse;
	uploads_pending = qfalse;
	descriptors_dirty = qtrue;
	num_samplers = 0;
	num_textures = 0;
	num_image_blocks = 0;
	image_bytes_used = 0;
	memset(device_textures, 0, sizeof(device_textures));
	memset(frame_fence_used, 0, sizeof(frame_fence_used));

	ResetFrameCommands();
	ResetUploadCommands();
	CreateFramebuffers();
	LoadShaders();

	dkRasterizerStateDefaults(&rasterizer);
	dkColorStateDefaults(&color_state);
	dkColorWriteStateDefaults(&color_write);
	dkBlendStateDefaults(&blend_state);
	dkDepthStencilStateDefaults(&depth_stencil);
	dkMultisampleStateDefaults(&multisample);
	rasterizer.cullMode = DkFace_None;
	depth_stencil.depthTestEnable = false;
	depth_stencil.depthWriteEnable = true;
	depth_stencil.depthCompareOp = DkCompareOp_Less;
	depth_stencil.stencilTestEnable = false;
	depth_stencil.stencilFrontFailOp = depth_stencil.stencilBackFailOp = DkStencilOp_Keep;
	depth_stencil.stencilFrontPassOp = depth_stencil.stencilBackPassOp = DkStencilOp_Keep;
	depth_stencil.stencilFrontDepthFailOp = depth_stencil.stencilBackDepthFailOp = DkStencilOp_Keep;
	memset(&render_state, 0xff, sizeof(render_state));
	memset(fog_color, 0, sizeof(fog_color));
	for (i = 0; i < DEV_MAX_TEXTURE_UNITS; i++)
		texture_handles[i] = 0;
	dirty = DIRTY_ALL;
	stat_last_ms = ri.Milliseconds();

	*width = screen_width;
	*height = screen_height;
	return qtrue;
}

void DEV_Shutdown (void)
{
	int	i;

	if (!device)
		return;
	dkQueueWaitIdle(queue);
	RunDeferredFrees(qtrue);

	dkSwapchainDestroy(swapchain);
	dkQueueDestroy(queue);
	dkCmdBufDestroy(cmd);
	dkCmdBufDestroy(upload_cmd);
	for (i = 0; i < num_image_blocks; i++)
	{
		dkMemBlockDestroy(image_blocks[i].block);
		free(image_blocks[i].ranges);
	}
	dkMemBlockDestroy(gamma_block);
	dkMemBlockDestroy(probe_block);
	dkMemBlockDestroy(uniform_block);
	dkMemBlockDestroy(descriptor_block);
	dkMemBlockDestroy(code_block);
	dkMemBlockDestroy(zero_block);
	dkMemBlockDestroy(stream_block);
	dkMemBlockDestroy(staging_block);
	dkMemBlockDestroy(upload_cmd_block);
	dkMemBlockDestroy(cmd_block);
	dkMemBlockDestroy(framebuffer_block);
	dkDeviceDestroy(device);
	device = NULL;
}
