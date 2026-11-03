/* fake_deko3d.c -- deko3D's interface, behaving as the GPU does where it matters here.
 *
 * source/renderer/dk_device.c runs on a PC against this, unchanged. What makes it worth
 * having is what a GPU does that a plain drawing function would not:
 *
 *  - Commands are recorded, and only run when their list is submitted.
 *  - State stays set across command lists and frames, until something changes it.
 *  - Images live in their memory blocks, at the offsets they were given, so that two
 *    allocations that overlap show it. Descriptors are read from memory when a draw
 *    needs them, and a block with no CPU access gives the CPU no address.
 *  - What the debug library would refuse is reported, with some of what a GPU would
 *    only get wrong silently: drawing to the wrong target, or sampling a frame that
 *    nothing has flushed.
 *
 * The drawing itself goes to soft_gpu.c, which rasterizes as the shaders would. What
 * this cannot show is that the console's GPU agrees: only a console can.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcommon/q_shared.h"

#include <deko3d.h>

#include "dk_device.h"
#include "soft_gpu.h"
#include "nx_platform.h"

#define MAGIC_IMAGE	0x474d4946
#define MAGIC_LAYOUT	0x4c594146
#define MAGIC_SHADER	0x52444846
#define MAGIC_DESC	0x43534446
#define MAX_REPORTED	24
#define BIG_PAGE	0x20000
#define MAX_VTX_BUFFERS	16
#define OP_COST		40		/* about what a command takes of a command buffer */

struct tag_DkDevice
{
	DkDeviceMaker	maker;
};

struct tag_DkMemBlock
{
	byte		*mem;
	uint32_t	size;
	uint32_t	flags;
};

struct tag_DkQueue
{
	int	unused;
};

struct tag_DkSwapchain
{
	int	num_images;
	int	next;
};

typedef enum
{
	OP_TARGETS, OP_BARRIER, OP_IMAGE_SET, OP_SAMPLER_SET, OP_MULTISAMPLE, OP_ATTRIBS, OP_BUFFERS, OP_UBO,
	OP_SHADERS, OP_RASTERIZER, OP_COLOR, OP_COLOR_WRITE, OP_BLEND, OP_DEPTH_STENCIL, OP_STENCIL, OP_ALPHA_REF,
	OP_VIEWPORT, OP_SCISSOR, OP_DEPTH_BIAS, OP_PUSH, OP_TEXTURE, OP_VTX_BUFFER, OP_IDX_BUFFER,
	OP_DRAW, OP_DRAW_INDEXED, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_DISCARD_DS,
	OP_COPY_TO_IMAGE, OP_COPY_FROM_IMAGE, OP_BLIT, OP_REPORT, OP_NOTHING
} opType_t;

typedef struct
{
	opType_t	type;
	union
	{
		struct { const void *color; const void *depth; int colors; } targets;
		struct { int mode; uint32_t flags; } barrier;
		DkGpuAddr	addr;
		struct { int count; } attribs;
		struct { uint32_t stride[MAX_VTX_BUFFERS]; } buffers;
		struct { int stage; DkGpuAddr addr; uint32_t size; } ubo;
		struct { const void *vertex; const void *fragment; } shaders;
		DkRasterizerState	rasterizer;
		DkColorState		color;
		DkColorWriteState	color_write;
		DkBlendState		blend;
		DkDepthStencilState	depth;
		struct { uint8_t mask, ref, func_mask; } stencil;
		float			alpha_ref;
		DkViewport		viewport;
		DkScissor		scissor;
		struct { float constant, slope; } bias;
		struct { DkGpuAddr addr; uint32_t offset, size; byte *data; } push;
		struct { int stage; uint32_t id; DkResHandle handle; } texture;
		struct { uint32_t id; DkGpuAddr addr; uint32_t size; } vtx;
		struct { int format; DkGpuAddr addr; } idx;
		struct { int prim; uint32_t count, first; } draw;
		struct { uint32_t mask; float rgba[4]; } clear_color;
		struct { bool depth; float value; uint8_t mask, stencil; } clear_ds;
		struct { DkCopyBuf buf; DkImageView view; DkImageRect rect; } copy;
		struct { DkImageView src, dst; DkImageRect src_rect, dst_rect; uint32_t flags; } blit;
		struct { int type; DkGpuAddr addr; } report;
	} u;
} op_t;

struct tag_DkCmdBuf
{
	DkCmdBufMaker	maker;
	op_t		*ops;
	int		count, capacity;
	uint32_t	memory;		/* what dkCmdBufAddMemory has given it */
	uint32_t	used;
};

typedef struct
{
	op_t	*ops;
	int	count;
} cmdList_t;

/* In a DkImageLayout's storage */
typedef struct
{
	uint32_t	magic;
	uint32_t	flags;
	uint32_t	format;
	uint32_t	width, height, levels;
	uint32_t	size, align;
} fakeLayout_t;

/* In a DkImage's storage */
typedef struct
{
	uint32_t	magic;
	fakeLayout_t	layout;
	byte		*pixels;
	int		is_swap;
} fakeImage_t;

/* In a DkImageDescriptor's 32 bytes: what the GPU reads when it samples */
typedef struct
{
	uint32_t	magic;
	uint32_t	has_alpha;
	byte		*pixels;
	uint16_t	width, height;
	uint16_t	levels;
} fakeImageDesc_t;

/* In a DkSamplerDescriptor's 32 bytes */
typedef struct
{
	uint32_t	magic;
	uint8_t		min, mag, mip, wrap_s, wrap_t;
	uint8_t		aniso;
} fakeSamplerDesc_t;

/* In a DkShader's storage. Which one it is comes from the order dk_device.c loads them in. */
typedef enum { SHADER_VERTEX, SHADER_PRESENT_VERTEX, SHADER_PRESENT_FRAGMENT, SHADER_FRAGMENT } shaderKind_t;

typedef struct
{
	uint32_t	magic;
	shaderKind_t	kind;
	int		program;
	int		clip;
} fakeShader_t;

/* The GPU's state: it stays until something changes it. */
static struct
{
	const fakeImage_t	*color, *depth;
	const fakeImageDesc_t	*image_set;
	uint32_t		image_set_count;
	const fakeSamplerDesc_t	*sampler_set;
	uint32_t		sampler_set_count;
	int			attrib_count;
	uint32_t		stride[MAX_VTX_BUFFERS];
	DkGpuAddr		vertex_ubo, fragment_ubo;
	const fakeShader_t	*vertex, *fragment;
	DkRasterizerState	rasterizer;
	DkColorState		color_state;
	DkColorWriteState	color_write;
	DkBlendState		blend;
	DkDepthStencilState	depth_state;
	uint8_t			stencil_mask, stencil_ref, stencil_func_mask;
	float			alpha_ref;
	DkViewport		viewport;
	DkScissor		scissor;
	float			bias_constant, bias_slope;
	DkResHandle		textures[2];
	DkGpuAddr		vbo[MAX_VTX_BUFFERS];
	DkGpuAddr		ibo;
	int			ibo_format;
	qboolean		frame_unflushed;	/* drawn to, with no barrier since */
	uint64_t		samples_passed;
} gpu;

static int	errors_seen;
static int	shaders_made;
static int	screen_w = 1280, screen_h = 720;
static uint64_t	frames_presented;

static void Report (const char *fmt, ...)
{
	char	text[512];
	va_list	ap;

	errors_seen++;
	if (errors_seen > MAX_REPORTED)
		return;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	NX_LogPrintf("FAKE DEKO3D ERROR (frame %llu): %s\n", (unsigned long long) frames_presented, text);
}

int FakeDeko3D_Errors (void)
{
	return errors_seen;
}

/* ---- device and memory ---- */

DkDevice dkDeviceCreate (DkDeviceMaker const *maker)
{
	DkDevice	device = calloc(1, sizeof(*device));

	device->maker = *maker;
	shaders_made = 0;
	memset(&gpu, 0, sizeof(gpu));
	return device;
}

void dkDeviceDestroy (DkDevice obj)
{
	free(obj);
}

DkMemBlock dkMemBlockCreate (DkMemBlockMaker const *maker)
{
	DkMemBlock	block = calloc(1, sizeof(*block));

	if (maker->size & (DK_MEMBLOCK_ALIGNMENT - 1))
		Report("dkMemBlockCreate: size %u is not aligned", maker->size);
	block->size = maker->size;
	block->flags = maker->flags;
	/* a GPU address is a pointer here, and memory blocks start on pages */
	if (posix_memalign((void **) &block->mem, DK_MEMBLOCK_ALIGNMENT, maker->size))
		Report("dkMemBlockCreate: no memory for %u bytes", maker->size);
	else
		memset(block->mem, 0, maker->size);
	return block;
}

void dkMemBlockDestroy (DkMemBlock obj)
{
	free(obj->mem);
	free(obj);
}

void *dkMemBlockGetCpuAddr (DkMemBlock obj)
{
	/* a block for the GPU alone gives the CPU nothing to touch */
	if (((obj->flags >> DkMemBlockFlags_CpuAccessShift) & DkMemAccess_Mask) == DkMemAccess_None)
		return NULL;
	return obj->mem;
}

DkGpuAddr dkMemBlockGetGpuAddr (DkMemBlock obj)
{
	return (DkGpuAddr) obj->mem;
}

/* ---- images ---- */

void dkImageLayoutInitialize (DkImageLayout *obj, DkImageLayoutMaker const *maker)
{
	fakeLayout_t	*l = (fakeLayout_t *) obj;
	uint32_t	w = maker->dimensions[0], h = maker->dimensions[1];
	uint32_t	levels = maker->mipLevels ? maker->mipLevels : 1;
	uint32_t	i, size = 0;

	memset(obj, 0, sizeof(*obj));
	l->magic = MAGIC_LAYOUT;
	l->flags = maker->flags;
	l->format = maker->format;
	l->width = w;
	l->height = h;
	l->levels = levels;
	for (i = 0; i < levels; i++)
	{
		size += w * h * 4;
		w = w > 1 ? w >> 1 : 1;
		h = h > 1 ? h >> 1 : 1;
	}
	l->align = (maker->flags & DkImageFlags_HwCompression) ? BIG_PAGE : 512;
	l->size = (size + l->align - 1) & ~(l->align - 1);
}

uint64_t dkImageLayoutGetSize (DkImageLayout const *obj)
{
	return ((const fakeLayout_t *) obj)->size;
}

uint32_t dkImageLayoutGetAlignment (DkImageLayout const *obj)
{
	return ((const fakeLayout_t *) obj)->align;
}

void dkImageInitialize (DkImage *obj, DkImageLayout const *layout, DkMemBlock block, uint32_t offset)
{
	fakeImage_t		*image = (fakeImage_t *) obj;
	const fakeLayout_t	*l = (const fakeLayout_t *) layout;

	memset(obj, 0, sizeof(*obj));
	image->magic = MAGIC_IMAGE;
	image->layout = *l;
	image->pixels = block->mem + offset;
	if (!(block->flags & DkMemBlockFlags_Image))
		Report("dkImageInitialize: the memory block is not for images");
	if (offset & (l->align - 1))
		Report("dkImageInitialize: offset %u is not aligned to %u", offset, l->align);
	if (offset + l->size > block->size)
		Report("dkImageInitialize: %u bytes at %u do not fit a block of %u", l->size, offset, block->size);
}

static const fakeImage_t *ImageOf (const DkImageView *view)
{
	const fakeImage_t	*image = (const fakeImage_t *) view->pImage;

	if (!image || image->magic != MAGIC_IMAGE)
		Report("an image view has no initialized image");
	return image;
}

/* Where a level starts, in the block: levels one after the other. */
static byte *LevelPixels (byte *pixels, uint32_t w, uint32_t h, uint32_t level, uint32_t *out_w, uint32_t *out_h)
{
	uint32_t	i;

	for (i = 0; i < level; i++)
	{
		pixels += (size_t) w * h * 4;
		w = w > 1 ? w >> 1 : 1;
		h = h > 1 ? h >> 1 : 1;
	}
	*out_w = w;
	*out_h = h;
	return pixels;
}

void dkImageDescriptorInitialize (DkImageDescriptor *obj, DkImageView const *view, bool usesLoadOrStore, bool decayMS)
{
	fakeImageDesc_t		*desc = (fakeImageDesc_t *) obj;
	const fakeImage_t	*image = ImageOf(view);

	(void) usesLoadOrStore;
	(void) decayMS;
	memset(obj, 0, sizeof(*obj));
	desc->magic = MAGIC_DESC;
	desc->pixels = image->pixels;
	desc->width = (uint16_t) image->layout.width;
	desc->height = (uint16_t) image->layout.height;
	desc->levels = (uint16_t) image->layout.levels;
	desc->has_alpha = view->swizzle[3] != DkImageSwizzle_One;
}

void dkSamplerDescriptorInitialize (DkSamplerDescriptor *obj, DkSampler const *sampler)
{
	fakeSamplerDesc_t	*desc = (fakeSamplerDesc_t *) obj;

	memset(obj, 0, sizeof(*obj));
	desc->magic = MAGIC_DESC;
	desc->min = sampler->minFilter;
	desc->mag = sampler->magFilter;
	desc->mip = sampler->mipFilter;
	desc->wrap_s = sampler->wrapMode[0];
	desc->wrap_t = sampler->wrapMode[1];
	desc->aniso = (uint8_t) sampler->maxAnisotropy;
}

/* ---- shaders ---- */

void dkShaderInitialize (DkShader *obj, DkShaderMaker const *maker)
{
	fakeShader_t	*shader = (fakeShader_t *) obj;
	const byte	*code = maker->codeMem->mem + maker->codeOffset;
	const int	n = shaders_made++;

	memset(obj, 0, sizeof(*obj));
	if (memcmp(code, "DKSH", 4))
	{
		Report("dkShaderInitialize: shader %d is not a DKSH file", n);
		return;
	}
	if (maker->codeOffset & (DK_SHADER_CODE_ALIGNMENT - 1))
		Report("dkShaderInitialize: shader %d is not aligned", n);
	if (!(maker->codeMem->flags & DkMemBlockFlags_Code))
		Report("dkShaderInitialize: shader %d is not in a block for code", n);

	shader->magic = MAGIC_SHADER;
	/* the order of LoadShaders in dk_device.c */
	if (n == 0)
		shader->kind = SHADER_VERTEX;
	else if (n == 1)
		shader->kind = SHADER_PRESENT_VERTEX;
	else if (n == 2)
		shader->kind = SHADER_PRESENT_FRAGMENT;
	else
	{
		shader->kind = SHADER_FRAGMENT;
		shader->program = (n - 3) / 2;
		shader->clip = (n - 3) % 2;
	}
}

bool dkShaderIsValid (DkShader const *obj)
{
	return ((const fakeShader_t *) obj)->magic == MAGIC_SHADER;
}

/* ---- command buffers ---- */

DkCmdBuf dkCmdBufCreate (DkCmdBufMaker const *maker)
{
	DkCmdBuf	buf = calloc(1, sizeof(*buf));

	buf->maker = *maker;
	return buf;
}

void dkCmdBufDestroy (DkCmdBuf obj)
{
	free(obj->ops);
	free(obj);
}

void dkCmdBufAddMemory (DkCmdBuf obj, DkMemBlock mem, uint32_t offset, uint32_t size)
{
	if (offset + size > mem->size)
		Report("dkCmdBufAddMemory: %u bytes at %u do not fit a block of %u", size, offset, mem->size);
	obj->memory += size;
}

void dkCmdBufClear (DkCmdBuf obj)
{
	int	i;

	for (i = 0; i < obj->count; i++)
		if (obj->ops[i].type == OP_PUSH)
			free(obj->ops[i].u.push.data);
	obj->count = 0;
	obj->memory = 0;
	obj->used = 0;
}

static op_t *Record (DkCmdBuf buf, opType_t type, uint32_t extra)
{
	op_t	*op;

	buf->used += OP_COST + extra;
	if (buf->used > buf->memory)
	{
		if (buf->maker.cbAddMem)
			buf->maker.cbAddMem(buf->maker.userData, buf, OP_COST + extra);
		if (buf->used > buf->memory)
			Report("a command buffer ran out of memory: %u of %u", buf->used, buf->memory);
	}
	if (buf->count == buf->capacity)
	{
		buf->capacity = buf->capacity ? buf->capacity * 2 : 1024;
		buf->ops = realloc(buf->ops, buf->capacity * sizeof(op_t));
	}
	op = &buf->ops[buf->count++];
	memset(op, 0, sizeof(*op));
	op->type = type;
	return op;
}

/* A finished list is its commands, copied: the buffer goes on from there. */
DkCmdList dkCmdBufFinishList (DkCmdBuf obj)
{
	cmdList_t	*list = malloc(sizeof(*list));
	int		i;

	list->count = obj->count;
	list->ops = malloc((obj->count ? obj->count : 1) * sizeof(op_t));
	memcpy(list->ops, obj->ops, obj->count * sizeof(op_t));
	for (i = 0; i < obj->count; i++)
		if (obj->ops[i].type == OP_PUSH)
			obj->ops[i].type = OP_NOTHING;	/* the list has its data now */
	obj->count = 0;
	return (DkCmdList) list;
}

void dkCmdBufSignalFence (DkCmdBuf obj, DkFence *fence, bool flush)
{
	(void) obj; (void) fence; (void) flush;
}

void dkCmdBufBarrier (DkCmdBuf obj, DkBarrier mode, uint32_t flags)
{
	op_t	*op = Record(obj, OP_BARRIER, 0);

	op->u.barrier.mode = mode;
	op->u.barrier.flags = flags;
}

void dkCmdBufBindRenderTargets (DkCmdBuf obj, DkImageView const *const colorTargets[], uint32_t numColorTargets, DkImageView const *depthTarget)
{
	op_t	*op = Record(obj, OP_TARGETS, 0);

	op->u.targets.colors = numColorTargets;
	op->u.targets.color = numColorTargets ? ImageOf(colorTargets[0]) : NULL;
	op->u.targets.depth = depthTarget ? ImageOf(depthTarget) : NULL;
}

void dkCmdBufBindImageDescriptorSet (DkCmdBuf obj, DkGpuAddr setAddr, uint32_t numDescriptors)
{
	op_t	*op = Record(obj, OP_IMAGE_SET, 0);

	if (setAddr & (DK_IMAGE_DESCRIPTOR_ALIGNMENT - 1))
		Report("dkCmdBufBindImageDescriptorSet: not aligned");
	op->u.ubo.addr = setAddr;
	op->u.ubo.size = numDescriptors;
}

void dkCmdBufBindSamplerDescriptorSet (DkCmdBuf obj, DkGpuAddr setAddr, uint32_t numDescriptors)
{
	op_t	*op = Record(obj, OP_SAMPLER_SET, 0);

	if (setAddr & (DK_SAMPLER_DESCRIPTOR_ALIGNMENT - 1))
		Report("dkCmdBufBindSamplerDescriptorSet: not aligned");
	op->u.ubo.addr = setAddr;
	op->u.ubo.size = numDescriptors;
}

void dkCmdBufBindMultisampleState (DkCmdBuf obj, DkMultisampleState const *state)
{
	(void) state;
	Record(obj, OP_MULTISAMPLE, 0);
}

void dkCmdBufBindVtxAttribState (DkCmdBuf obj, DkVtxAttribState const attribs[], uint32_t numAttribs)
{
	(void) attribs;
	Record(obj, OP_ATTRIBS, 0)->u.attribs.count = numAttribs;
}

void dkCmdBufBindVtxBufferState (DkCmdBuf obj, DkVtxBufferState const buffers[], uint32_t numBuffers)
{
	op_t		*op = Record(obj, OP_BUFFERS, 0);
	uint32_t	i;

	for (i = 0; i < numBuffers; i++)
		op->u.buffers.stride[i] = buffers[i].stride;
}

void dkCmdBufBindUniformBuffers (DkCmdBuf obj, DkStage stage, uint32_t firstId, DkBufExtents const buffers[], uint32_t numBuffers)
{
	op_t	*op = Record(obj, OP_UBO, 0);

	(void) firstId;
	(void) numBuffers;
	if (buffers[0].addr & (DK_UNIFORM_BUF_ALIGNMENT - 1))
		Report("dkCmdBufBindUniformBuffer: the address is not aligned");
	if (buffers[0].size & (DK_UNIFORM_BUF_ALIGNMENT - 1))
		Report("dkCmdBufBindUniformBuffer: the size is not aligned");
	op->u.ubo.stage = stage;
	op->u.ubo.addr = buffers[0].addr;
	op->u.ubo.size = buffers[0].size;
}

void dkCmdBufBindShaders (DkCmdBuf obj, uint32_t stageMask, DkShader const *const shaders[], uint32_t numShaders)
{
	op_t		*op = Record(obj, OP_SHADERS, 0);
	uint32_t	i;

	(void) stageMask;
	for (i = 0; i < numShaders; i++)
	{
		const fakeShader_t	*s = (const fakeShader_t *) shaders[i];

		if (s->magic != MAGIC_SHADER)
			Report("dkCmdBufBindShaders: an invalid shader");
		else if (s->kind == SHADER_VERTEX || s->kind == SHADER_PRESENT_VERTEX)
			op->u.shaders.vertex = s;
		else
			op->u.shaders.fragment = s;
	}
}

void dkCmdBufBindRasterizerState (DkCmdBuf obj, DkRasterizerState const *state)
{
	Record(obj, OP_RASTERIZER, 0)->u.rasterizer = *state;
}

void dkCmdBufBindColorState (DkCmdBuf obj, DkColorState const *state)
{
	Record(obj, OP_COLOR, 0)->u.color = *state;
}

void dkCmdBufBindColorWriteState (DkCmdBuf obj, DkColorWriteState const *state)
{
	Record(obj, OP_COLOR_WRITE, 0)->u.color_write = *state;
}

void dkCmdBufBindBlendStates (DkCmdBuf obj, uint32_t firstId, DkBlendState const states[], uint32_t numStates)
{
	(void) firstId;
	(void) numStates;
	Record(obj, OP_BLEND, 0)->u.blend = states[0];
}

void dkCmdBufBindDepthStencilState (DkCmdBuf obj, DkDepthStencilState const *state)
{
	Record(obj, OP_DEPTH_STENCIL, 0)->u.depth = *state;
}

void dkCmdBufSetStencil (DkCmdBuf obj, DkFace face, uint8_t mask, uint8_t funcRef, uint8_t funcMask)
{
	op_t	*op = Record(obj, OP_STENCIL, 0);

	(void) face;
	op->u.stencil.mask = mask;
	op->u.stencil.ref = funcRef;
	op->u.stencil.func_mask = funcMask;
}

void dkCmdBufSetAlphaRef (DkCmdBuf obj, float ref)
{
	Record(obj, OP_ALPHA_REF, 0)->u.alpha_ref = ref;
}

void dkCmdBufSetViewports (DkCmdBuf obj, uint32_t firstId, DkViewport const viewports[], uint32_t numViewports)
{
	(void) firstId;
	(void) numViewports;
	Record(obj, OP_VIEWPORT, 0)->u.viewport = viewports[0];
}

void dkCmdBufSetScissors (DkCmdBuf obj, uint32_t firstId, DkScissor const scissors[], uint32_t numScissors)
{
	(void) firstId;
	(void) numScissors;
	Record(obj, OP_SCISSOR, 0)->u.scissor = scissors[0];
}

void dkCmdBufSetDepthBias (DkCmdBuf obj, float constantFactor, float clamp, float slopeFactor)
{
	op_t	*op = Record(obj, OP_DEPTH_BIAS, 0);

	(void) clamp;
	op->u.bias.constant = constantFactor;
	op->u.bias.slope = slopeFactor;
}

void dkCmdBufPushConstants (DkCmdBuf obj, DkGpuAddr uboAddr, uint32_t uboSize, uint32_t offset, uint32_t size, const void *data)
{
	op_t	*op = Record(obj, OP_PUSH, size);

	if ((uboAddr & (DK_UNIFORM_BUF_ALIGNMENT - 1)) || (uboSize & (DK_UNIFORM_BUF_ALIGNMENT - 1))
			|| (offset & 3) || (size & 3) || offset + size > uboSize)
		Report("dkCmdBufPushConstants: bad address, offset or size (%u+%u in %u)", offset, size, uboSize);
	op->u.push.addr = uboAddr;
	op->u.push.offset = offset;
	op->u.push.size = size;
	op->u.push.data = malloc(size);
	memcpy(op->u.push.data, data, size);
}

void dkCmdBufBindTextures (DkCmdBuf obj, DkStage stage, uint32_t firstId, DkResHandle const handles[], uint32_t numHandles)
{
	op_t	*op = Record(obj, OP_TEXTURE, 0);

	(void) numHandles;
	op->u.texture.stage = stage;
	op->u.texture.id = firstId;
	op->u.texture.handle = handles[0];
}

void dkCmdBufBindVtxBuffers (DkCmdBuf obj, uint32_t firstId, DkBufExtents const buffers[], uint32_t numBuffers)
{
	op_t	*op = Record(obj, OP_VTX_BUFFER, 0);

	(void) numBuffers;
	op->u.vtx.id = firstId;
	op->u.vtx.addr = buffers[0].addr;
	op->u.vtx.size = buffers[0].size;
}

void dkCmdBufBindIdxBuffer (DkCmdBuf obj, DkIdxFormat format, DkGpuAddr address)
{
	op_t	*op = Record(obj, OP_IDX_BUFFER, 0);

	op->u.idx.format = format;
	op->u.idx.addr = address;
}

void dkCmdBufDraw (DkCmdBuf obj, DkPrimitive prim, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
	op_t	*op = Record(obj, OP_DRAW, 0);

	(void) instanceCount;
	(void) firstInstance;
	op->u.draw.prim = prim;
	op->u.draw.count = vertexCount;
	op->u.draw.first = firstVertex;
}

void dkCmdBufDrawIndexed (DkCmdBuf obj, DkPrimitive prim, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
	op_t	*op = Record(obj, OP_DRAW_INDEXED, 0);

	(void) instanceCount;
	(void) vertexOffset;
	(void) firstInstance;
	op->u.draw.prim = prim;
	op->u.draw.count = indexCount;
	op->u.draw.first = firstIndex;
}

void dkCmdBufClearColor (DkCmdBuf obj, uint32_t targetId, uint32_t clearMask, const void *clearData)
{
	op_t	*op = Record(obj, OP_CLEAR_COLOR, 0);

	(void) targetId;
	op->u.clear_color.mask = clearMask;
	memcpy(op->u.clear_color.rgba, clearData, sizeof(op->u.clear_color.rgba));
}

void dkCmdBufClearDepthStencil (DkCmdBuf obj, bool clearDepth, float depthValue, uint8_t stencilMask, uint8_t stencilValue)
{
	op_t	*op = Record(obj, OP_CLEAR_DS, 0);

	op->u.clear_ds.depth = clearDepth;
	op->u.clear_ds.value = depthValue;
	op->u.clear_ds.mask = stencilMask;
	op->u.clear_ds.stencil = stencilValue;
}

void dkCmdBufDiscardDepthStencil (DkCmdBuf obj)
{
	Record(obj, OP_DISCARD_DS, 0);
}

void dkCmdBufCopyBufferToImage (DkCmdBuf obj, DkCopyBuf const *src, DkImageView const *dstView, DkImageRect const *dstRect, uint32_t flags)
{
	op_t	*op = Record(obj, OP_COPY_TO_IMAGE, 0);

	(void) flags;
	op->u.copy.buf = *src;
	op->u.copy.view = *dstView;
	op->u.copy.rect = *dstRect;
}

void dkCmdBufCopyImageToBuffer (DkCmdBuf obj, DkImageView const *srcView, DkImageRect const *srcRect, DkCopyBuf const *dst, uint32_t flags)
{
	op_t	*op = Record(obj, OP_COPY_FROM_IMAGE, 0);

	(void) flags;
	op->u.copy.buf = *dst;
	op->u.copy.view = *srcView;
	op->u.copy.rect = *srcRect;
}

void dkCmdBufBlitImage (DkCmdBuf obj, DkImageView const *srcView, DkImageRect const *srcRect, DkImageView const *dstView, DkImageRect const *dstRect, uint32_t flags, uint32_t factor)
{
	op_t	*op = Record(obj, OP_BLIT, 0);

	(void) factor;
	op->u.blit.src = *srcView;
	op->u.blit.dst = *dstView;
	op->u.blit.src_rect = *srcRect;
	op->u.blit.dst_rect = *dstRect;
	op->u.blit.flags = flags;
}

void dkCmdBufReportCounter (DkCmdBuf obj, DkCounter type, DkGpuAddr addr)
{
	op_t	*op = Record(obj, OP_REPORT, 0);

	op->u.report.type = type;
	op->u.report.addr = addr;
}

/* ---- running a list: where a draw becomes pixels ---- */

static GLenum CompareToGL (DkCompareOp op)
{
	switch (op)
	{
	case DkCompareOp_Never:		return GL_NEVER;
	case DkCompareOp_Less:		return GL_LESS;
	case DkCompareOp_Equal:		return GL_EQUAL;
	case DkCompareOp_Lequal:	return GL_LEQUAL;
	case DkCompareOp_Greater:	return GL_GREATER;
	case DkCompareOp_NotEqual:	return GL_NOTEQUAL;
	case DkCompareOp_Gequal:	return GL_GEQUAL;
	default:			return GL_ALWAYS;
	}
}

static GLenum FactorToGL (DkBlendFactor factor)
{
	switch (factor)
	{
	case DkBlendFactor_Zero:		return GL_ZERO;
	case DkBlendFactor_One:			return GL_ONE;
	case DkBlendFactor_SrcColor:		return GL_SRC_COLOR;
	case DkBlendFactor_InvSrcColor:		return GL_ONE_MINUS_SRC_COLOR;
	case DkBlendFactor_SrcAlpha:		return GL_SRC_ALPHA;
	case DkBlendFactor_InvSrcAlpha:		return GL_ONE_MINUS_SRC_ALPHA;
	case DkBlendFactor_DstAlpha:		return GL_DST_ALPHA;
	case DkBlendFactor_InvDstAlpha:		return GL_ONE_MINUS_DST_ALPHA;
	case DkBlendFactor_DstColor:		return GL_DST_COLOR;
	case DkBlendFactor_InvDstColor:		return GL_ONE_MINUS_DST_COLOR;
	case DkBlendFactor_SrcAlphaSaturate:	return GL_SRC_ALPHA_SATURATE;
	default:				return GL_ONE;
	}
}

static GLenum StencilToGL (DkStencilOp op)
{
	switch (op)
	{
	case DkStencilOp_Zero:		return GL_ZERO;
	case DkStencilOp_Replace:	return GL_REPLACE;
	case DkStencilOp_Incr:		return GL_INCR;
	case DkStencilOp_Decr:		return GL_DECR;
	case DkStencilOp_Invert:	return GL_INVERT;
	default:			return GL_KEEP;
	}
}

/* The GPU's state, as what soft_gpu.c takes. The screen's first row is its top here. */
static void SyncRenderState (void)
{
	devRenderState_t	s;

	memset(&s, 0, sizeof(s));
	s.blend = (gpu.color_state.blendEnableMask & 1) != 0;
	s.blend_src = FactorToGL(gpu.blend.srcColorBlendFactor);
	s.blend_dst = FactorToGL(gpu.blend.dstColorBlendFactor);
	s.alpha_test = gpu.color_state.alphaCompareOp != DkCompareOp_Always;
	s.alpha_func = CompareToGL(gpu.color_state.alphaCompareOp);
	s.alpha_ref = gpu.alpha_ref;
	s.depth_test = gpu.depth_state.depthTestEnable;
	s.depth_write = gpu.depth_state.depthWriteEnable;
	s.depth_func = CompareToGL(gpu.depth_state.depthCompareOp);
	s.depth_near = gpu.viewport.near;
	s.depth_far = gpu.viewport.far;
	s.cull = gpu.rasterizer.cullMode != DkFace_None;
	s.cull_face = gpu.rasterizer.cullMode == DkFace_Front ? GL_FRONT
			: (gpu.rasterizer.cullMode == DkFace_Back ? GL_BACK : GL_FRONT_AND_BACK);
	s.polygon_offset = (gpu.rasterizer.depthBiasEnableMask & DkPolygonFlag_Fill) != 0;
	s.offset_factor = gpu.bias_slope;
	s.offset_units = gpu.bias_constant;
	s.polygon_line = gpu.rasterizer.polygonModeFront == DkPolygonMode_Line;
	s.color_mask = gpu.color_write.masks & 15;
	s.stencil_test = gpu.depth_state.stencilTestEnable;
	s.stencil_func = CompareToGL(gpu.depth_state.stencilFrontCompareOp);
	s.stencil_ref = gpu.stencil_ref;
	s.stencil_mask = gpu.stencil_func_mask;
	s.stencil_write_mask = gpu.stencil_mask;
	s.stencil_fail = StencilToGL(gpu.depth_state.stencilFrontFailOp);
	s.stencil_zfail = StencilToGL(gpu.depth_state.stencilFrontDepthFailOp);
	s.stencil_zpass = StencilToGL(gpu.depth_state.stencilFrontPassOp);
	s.scissor_test = GL_TRUE;
	s.scissor[0] = gpu.scissor.x;
	s.scissor[1] = screen_h - (int) gpu.scissor.y - (int) gpu.scissor.height;
	s.scissor[2] = gpu.scissor.width;
	s.scissor[3] = gpu.scissor.height;
	s.viewport[0] = (int) gpu.viewport.x;
	s.viewport[1] = screen_h - (int) gpu.viewport.y - (int) gpu.viewport.height;
	s.viewport[2] = (int) gpu.viewport.width;
	s.viewport[3] = (int) gpu.viewport.height;
	SOFT_SetRenderState(&s);
}

/* What a texture handle comes to, read from the descriptors as they are now. */
static void BindUnit (int unit)
{
	const DkResHandle	handle = gpu.textures[unit];
	const uint32_t		image_id = handle & ((1u << 20) - 1);
	const uint32_t		sampler_id = handle >> 20;
	const fakeImageDesc_t	*image;
	const fakeSamplerDesc_t	*sampler;
	softTexture_t		tex;
	devSampler_t		s;
	uint32_t		w, h, level;

	if (!gpu.image_set || image_id >= gpu.image_set_count || !gpu.sampler_set || sampler_id >= gpu.sampler_set_count)
	{
		Report("a draw reads texture unit %d, whose handle (%u, %u) is out of the descriptor sets", unit, image_id, sampler_id);
		SOFT_BindTexture(unit, NULL, NULL);
		return;
	}
	image = (const fakeImageDesc_t *) ((const byte *) gpu.image_set + image_id * sizeof(DkImageDescriptor));
	sampler = (const fakeSamplerDesc_t *) ((const byte *) gpu.sampler_set + sampler_id * sizeof(DkSamplerDescriptor));
	if (image->magic != MAGIC_DESC || sampler->magic != MAGIC_DESC)
	{
		Report("a draw reads texture unit %d, whose descriptors (%u, %u) were never written", unit, image_id, sampler_id);
		SOFT_BindTexture(unit, NULL, NULL);
		return;
	}

	memset(&tex, 0, sizeof(tex));
	tex.has_alpha = image->has_alpha;
	tex.levels = image->levels > SOFT_MAX_LEVELS ? SOFT_MAX_LEVELS : image->levels;
	for (level = 0; level < (uint32_t) tex.levels; level++)
	{
		tex.pixels[level] = LevelPixels(image->pixels, image->width, image->height, level, &w, &h);
		tex.w[level] = w;
		tex.h[level] = h;
	}

	s.min_filter = sampler->mip == DkMipFilter_None
			? (sampler->min == DkFilter_Nearest ? GL_NEAREST : GL_LINEAR)
			: GL_LINEAR_MIPMAP_LINEAR;
	s.mag_filter = sampler->mag == DkFilter_Nearest ? GL_NEAREST : GL_LINEAR;
	s.wrap_s = sampler->wrap_s == DkWrapMode_Repeat ? GL_REPEAT : GL_CLAMP;
	s.wrap_t = sampler->wrap_t == DkWrapMode_Repeat ? GL_REPEAT : GL_CLAMP;
	s.anisotropy = sampler->aniso;
	SOFT_BindTexture(unit, &tex, &s);
}

static GLenum PrimitiveToGL (int prim)
{
	switch (prim)
	{
	case DkPrimitive_Points:	return GL_POINTS;
	case DkPrimitive_Lines:		return GL_LINES;
	case DkPrimitive_LineLoop:	return GL_LINE_LOOP;
	case DkPrimitive_LineStrip:	return GL_LINE_STRIP;
	case DkPrimitive_TriangleStrip:	return GL_TRIANGLE_STRIP;
	case DkPrimitive_TriangleFan:	return GL_TRIANGLE_FAN;
	case DkPrimitive_Quads:		return GL_QUADS;
	case DkPrimitive_QuadStrip:	return GL_QUAD_STRIP;
	case DkPrimitive_Polygon:	return GL_POLYGON;
	default:			return GL_TRIANGLES;
	}
}

/* The present shaders: the frame's image, through the gamma table in texture unit 1. */
static void RunPresent (void)
{
	const uint32_t		lut_id = gpu.textures[1] & ((1u << 20) - 1);
	const fakeImageDesc_t	*lut = gpu.image_set
			? (const fakeImageDesc_t *) ((const byte *) gpu.image_set + lut_id * sizeof(DkImageDescriptor)) : NULL;

	if (gpu.color && !gpu.color->is_swap)
		Report("the present shaders draw to something other than a screen image");
	if (gpu.attrib_count != 0)
		Report("the present shaders draw with %d vertex attributes bound", gpu.attrib_count);
	if (gpu.frame_unflushed)
		Report("the frame is sampled with no barrier since it was drawn");
	if (gpu.depth_state.depthTestEnable)
		Report("the present pass has the depth test on");
	if (!lut || lut->magic != MAGIC_DESC || lut->width != 256)
	{
		Report("the present pass has no gamma table bound");
		return;
	}
	SOFT_ApplyGamma(lut->pixels);
}

static void RunDraw (const op_t *op, qboolean indexed)
{
	const GLenum		mode = PrimitiveToGL(op->u.draw.prim);
	const fakeShader_t	*vs = gpu.vertex, *fs = gpu.fragment;
	devVertices_t		v;

	if (!vs || !fs)
	{
		Report("a draw with no shaders bound");
		return;
	}

	if (fs->kind == SHADER_PRESENT_FRAGMENT || vs->kind == SHADER_PRESENT_VERTEX)
	{
		if (indexed || op->u.draw.count != 3 || vs->kind != SHADER_PRESENT_VERTEX || fs->kind != SHADER_PRESENT_FRAGMENT)
			Report("a draw of %u %s uses the present shaders: the game's geometry through the wrong program",
					op->u.draw.count, indexed ? "indices" : "vertices");
		RunPresent();
		return;
	}

	if (gpu.color && gpu.color->is_swap)
		Report("the game draws straight onto a screen image");
	if (!gpu.color)
		Report("a draw with no color target");
	if (gpu.attrib_count != 4)
		Report("a draw with %d vertex attributes bound", gpu.attrib_count);
	if (!gpu.vertex_ubo || !gpu.fragment_ubo)
	{
		Report("a draw with no uniform buffers bound");
		return;
	}
	if (gpu.stride[0] != 16 || gpu.stride[1] != 4 || gpu.stride[2] != 8 || gpu.stride[3] != 8)
		Report("a draw with the wrong vertex strides: %u %u %u %u", gpu.stride[0], gpu.stride[1], gpu.stride[2], gpu.stride[3]);
	if (!gpu.vbo[0])
		Report("a draw with no position buffer");

	SyncRenderState();
	SOFT_SetProgram((devProgram_t) fs->program, fs->clip);
	SOFT_SetVertexUniforms((const devVertexUniforms_t *) gpu.vertex_ubo);
	SOFT_SetFogColor((const float *) gpu.fragment_ubo);
	v.pos = gpu.vbo[0];
	v.color = gpu.vbo[1];
	v.st0 = gpu.vbo[2];
	v.st1 = gpu.vbo[3];
	SOFT_SetVertices(&v);

	if (fs->program != DEV_FS_COLOR)
		BindUnit(0);
	if (fs->program >= DEV_FS_MULTI_MODULATE)
		BindUnit(1);

	if (indexed)
	{
		if (gpu.ibo_format != DkIdxFormat_Uint32 || !gpu.ibo)
			Report("an indexed draw with no 32 bit index buffer");
		SOFT_DrawIndexed(mode, gpu.ibo + op->u.draw.first * 4, op->u.draw.count);
	}
	else
	{
		if (mode == GL_POINTS && SOFT_PointPasses())
			gpu.samples_passed++;
		SOFT_Draw(mode, op->u.draw.first, op->u.draw.count);
	}
	gpu.frame_unflushed = qtrue;
}

static void RunCopyToImage (const op_t *op)
{
	const fakeImage_t	*image = ImageOf(&op->u.copy.view);
	const DkImageRect	*r = &op->u.copy.rect;
	const uint32_t		stride = op->u.copy.buf.rowLength ? op->u.copy.buf.rowLength : r->width * 4;
	uint32_t		w, h, row;
	byte			*dst = LevelPixels(image->pixels, image->layout.width, image->layout.height,
						op->u.copy.view.mipLevelOffset, &w, &h);
	const byte		*src = (const byte *) op->u.copy.buf.addr;

	if (op->u.copy.view.mipLevelOffset >= image->layout.levels || r->x + r->width > w || r->y + r->height > h)
	{
		Report("a copy of %ux%u at %u,%u does not fit level %u, of %ux%u", r->width, r->height, r->x, r->y,
				op->u.copy.view.mipLevelOffset, w, h);
		return;
	}
	for (row = 0; row < r->height; row++)
		memcpy(dst + ((size_t) (r->y + row) * w + r->x) * 4, src + (size_t) row * stride, r->width * 4);
}

static void RunBlit (const op_t *op)
{
	const fakeImage_t	*dst = ImageOf(&op->u.blit.dst);
	const DkImageRect	*sr = &op->u.blit.src_rect;
	uint32_t		row;

	/* the one blit there is: the frame's image to a plain one, for a read back */
	for (row = 0; row < sr->height; row++)
	{
		const uint32_t	src_row = (op->u.blit.flags & DkBlitFlag_FlipY) ? sr->y + sr->height - 1 - row : sr->y + row;

		SOFT_ReadRow(src_row, sr->x, sr->width, dst->pixels + (size_t) (op->u.blit.dst_rect.y + row) * dst->layout.width * 4);
	}
}

static void RunCopyFromImage (const op_t *op)
{
	const fakeImage_t	*image = ImageOf(&op->u.copy.view);
	const DkImageRect	*r = &op->u.copy.rect;
	const uint32_t		stride = op->u.copy.buf.rowLength ? op->u.copy.buf.rowLength : r->width * 4;
	byte			*out = (byte *) op->u.copy.buf.addr;
	uint32_t		row;

	for (row = 0; row < r->height; row++)
		memcpy(out + (size_t) row * stride, image->pixels + ((size_t) (r->y + row) * image->layout.width + r->x) * 4, r->width * 4);
}

static void RunList (const cmdList_t *list)
{
	int	i;

	for (i = 0; i < list->count; i++)
	{
		const op_t	*op = &list->ops[i];

		switch (op->type)
		{
		case OP_TARGETS:
			gpu.color = op->u.targets.color;
			gpu.depth = op->u.targets.depth;
			break;
		case OP_BARRIER:
			if ((op->u.barrier.mode == DkBarrier_Tiles || op->u.barrier.mode == DkBarrier_Fragments
					|| op->u.barrier.mode == DkBarrier_Primitives || op->u.barrier.mode == DkBarrier_Full)
					&& (op->u.barrier.flags & DkInvalidateFlags_Image))
				gpu.frame_unflushed = qfalse;
			break;
		case OP_IMAGE_SET:
			gpu.image_set = (const fakeImageDesc_t *) op->u.ubo.addr;
			gpu.image_set_count = op->u.ubo.size;
			break;
		case OP_SAMPLER_SET:
			gpu.sampler_set = (const fakeSamplerDesc_t *) op->u.ubo.addr;
			gpu.sampler_set_count = op->u.ubo.size;
			break;
		case OP_MULTISAMPLE:
			break;
		case OP_ATTRIBS:
			gpu.attrib_count = op->u.attribs.count;
			break;
		case OP_BUFFERS:
			memcpy(gpu.stride, op->u.buffers.stride, sizeof(gpu.stride));
			break;
		case OP_UBO:
			if (op->u.ubo.stage == DkStage_Vertex)
				gpu.vertex_ubo = op->u.ubo.addr;
			else
				gpu.fragment_ubo = op->u.ubo.addr;
			break;
		case OP_SHADERS:
			if (op->u.shaders.vertex)
				gpu.vertex = op->u.shaders.vertex;
			if (op->u.shaders.fragment)
				gpu.fragment = op->u.shaders.fragment;
			break;
		case OP_RASTERIZER:
			gpu.rasterizer = op->u.rasterizer;
			break;
		case OP_COLOR:
			gpu.color_state = op->u.color;
			break;
		case OP_COLOR_WRITE:
			gpu.color_write = op->u.color_write;
			break;
		case OP_BLEND:
			gpu.blend = op->u.blend;
			break;
		case OP_DEPTH_STENCIL:
			gpu.depth_state = op->u.depth;
			break;
		case OP_STENCIL:
			gpu.stencil_mask = op->u.stencil.mask;
			gpu.stencil_ref = op->u.stencil.ref;
			gpu.stencil_func_mask = op->u.stencil.func_mask;
			break;
		case OP_ALPHA_REF:
			gpu.alpha_ref = op->u.alpha_ref;
			break;
		case OP_VIEWPORT:
			gpu.viewport = op->u.viewport;
			break;
		case OP_SCISSOR:
			gpu.scissor = op->u.scissor;
			break;
		case OP_DEPTH_BIAS:
			gpu.bias_constant = op->u.bias.constant;
			gpu.bias_slope = op->u.bias.slope;
			break;
		case OP_PUSH:
			memcpy((byte *) op->u.push.addr + op->u.push.offset, op->u.push.data, op->u.push.size);
			break;
		case OP_TEXTURE:
			if (op->u.texture.stage == DkStage_Fragment && op->u.texture.id < 2)
				gpu.textures[op->u.texture.id] = op->u.texture.handle;
			break;
		case OP_VTX_BUFFER:
			if (op->u.vtx.id < MAX_VTX_BUFFERS)
				gpu.vbo[op->u.vtx.id] = op->u.vtx.addr;
			break;
		case OP_IDX_BUFFER:
			gpu.ibo = op->u.idx.addr;
			gpu.ibo_format = op->u.idx.format;
			break;
		case OP_DRAW:
			RunDraw(op, qfalse);
			break;
		case OP_DRAW_INDEXED:
			RunDraw(op, qtrue);
			break;
		case OP_CLEAR_COLOR:
			SyncRenderState();
			SOFT_ClearColor(op->u.clear_color.mask, op->u.clear_color.rgba);
			gpu.frame_unflushed = qtrue;
			break;
		case OP_CLEAR_DS:
			SyncRenderState();
			if (!gpu.depth)
				Report("a depth clear with no depth target");
			SOFT_ClearDepthStencil(op->u.clear_ds.depth, op->u.clear_ds.value, op->u.clear_ds.mask, op->u.clear_ds.stencil);
			break;
		case OP_DISCARD_DS:
			break;
		case OP_COPY_TO_IMAGE:
			RunCopyToImage(op);
			break;
		case OP_COPY_FROM_IMAGE:
			RunCopyFromImage(op);
			break;
		case OP_BLIT:
			RunBlit(op);
			break;
		case OP_REPORT:
			((uint64_t *) op->u.report.addr)[0] = gpu.samples_passed;
			((uint64_t *) op->u.report.addr)[1] = frames_presented;
			break;
		default:
			break;
		}
	}
}

/* ---- queues, fences and the screen: everything is done by the time a call returns ---- */

DkQueue dkQueueCreate (DkQueueMaker const *maker)
{
	(void) maker;
	return calloc(1, sizeof(struct tag_DkQueue));
}

void dkQueueDestroy (DkQueue obj)
{
	free(obj);
}

bool dkQueueIsInErrorState (DkQueue obj)
{
	(void) obj;
	return false;
}

void dkQueueSubmitCommands (DkQueue obj, DkCmdList cmds)
{
	cmdList_t	*list = (cmdList_t *) cmds;
	int		i;

	(void) obj;
	RunList(list);
	for (i = 0; i < list->count; i++)
		if (list->ops[i].type == OP_PUSH)
			free(list->ops[i].u.push.data);
	free(list->ops);
	free(list);
}

void dkQueueSignalFence (DkQueue obj, DkFence *fence, bool flush)
{
	(void) obj; (void) fence; (void) flush;
}

void dkQueueFlush (DkQueue obj)
{
	(void) obj;
}

void dkQueueWaitIdle (DkQueue obj)
{
	(void) obj;
}

DkResult dkFenceWait (DkFence *obj, int64_t timeout_ns)
{
	(void) obj; (void) timeout_ns;
	return DkResult_Success;
}

DkSwapchain dkSwapchainCreate (DkSwapchainMaker const *maker)
{
	DkSwapchain	swap = calloc(1, sizeof(*swap));
	uint32_t	i;

	swap->num_images = maker->numImages;
	for (i = 0; i < maker->numImages; i++)
	{
		fakeImage_t	*image = (fakeImage_t *) maker->pImages[i];

		image->is_swap = 1;
		screen_w = image->layout.width;
		screen_h = image->layout.height;
		if (!(image->layout.flags & DkImageFlags_UsagePresent))
			Report("a screen image has no DkImageFlags_UsagePresent");
	}
	return swap;
}

void dkSwapchainDestroy (DkSwapchain obj)
{
	free(obj);
}

int dkQueueAcquireImage (DkQueue obj, DkSwapchain swapchain)
{
	const int	slot = swapchain->next;

	(void) obj;
	swapchain->next = (swapchain->next + 1) % swapchain->num_images;
	return slot;
}

void dkQueuePresentImage (DkQueue obj, DkSwapchain swapchain, int imageSlot)
{
	(void) obj; (void) swapchain; (void) imageSlot;
	frames_presented++;
	SOFT_Present();
}
