/* snd_nx.c -- sound driver on libnx's audout service.
 *
 * The engine mixes ahead into a ring (dma.buffer), as it does on a sound card.
 * A thread of ours plays the part of the card's DMA: it copies from the ring to
 * the buffers the service plays, and that copying is what moves the position.
 *
 * audout plays 48 kHz and nothing else. The game mixes at half of that, which is
 * about what it does by default on a PC (22 kHz) and keeps its sounds and the work
 * of mixing them at half the size, and the thread doubles the samples.
 */

#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "client/snd_local.h"

#define NX_OUT_RATE		48000
#define NX_MIX_RATE		(NX_OUT_RATE / 2)
#define NX_CHANNELS		2

#define NX_RING_SAMPLES		(1 << 15)	/* power of two, ~680 ms */
#define NX_CHUNK_FRAMES		1024		/* 21 ms of output: one page of memory */
#define NX_CHUNK_BYTES		(NX_CHUNK_FRAMES * NX_CHANNELS * sizeof(s16))
#define NX_CHUNKS		3

#define NX_THREAD_STACK		0x4000
#define NX_THREAD_PRIO		0x2B		/* above the engine's 0x2C */
#define NX_THREAD_CORE		2
#define NX_WAIT_NS		20000000ULL

static Thread		snd_thread;
static AudioOutBuffer	chunks[NX_CHUNKS];
static void		*chunk_mem;
static s16		*ring;
static volatile bool	snd_running;
static volatile int	dma_pos;
static s16		last_left, last_right;

/* Each mixed frame becomes two, the first halfway from the one before it. */
static void FillChunk (AudioOutBuffer *chunk)
{
	s16	*out = (s16 *) chunk->buffer;
	int	pos = dma_pos;
	int	i;

	for (i = 0; i < NX_CHUNK_FRAMES / 2; i++)
	{
		const s16	left = ring[pos];
		const s16	right = ring[pos + 1];

		out[0] = (last_left + left) >> 1;
		out[1] = (last_right + right) >> 1;
		out[2] = left;
		out[3] = right;
		out += 4;
		last_left = left;
		last_right = right;
		pos = (pos + NX_CHANNELS) & (NX_RING_SAMPLES - 1);
	}
	dma_pos = pos;

	armDCacheFlush(chunk->buffer, NX_CHUNK_BYTES);
}

static void SoundThread (void *unused)
{
	(void) unused;

	while (snd_running)
	{
		AudioOutBuffer	*released = NULL;
		u32		count = 0;

		if (R_FAILED(audoutWaitPlayFinish(&released, &count, NX_WAIT_NS)))
			continue;

		/* The wait hands over one buffer, and more may have played. */
		while (count > 0 && released)
		{
			FillChunk(released);
			audoutAppendAudioOutBuffer(released);
			released = NULL;
			count = 0;
			if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)))
				break;
		}
	}
}

static void FreeMemory (void)
{
	free(chunk_mem);
	chunk_mem = NULL;
	free(ring);
	ring = NULL;
}

qboolean SNDDMA_Init (void)
{
	Result	rc;
	int	i;

	if (ring)
		return qtrue;

	rc = audoutInitialize();
	if (R_FAILED(rc))
	{
		Com_Printf("audoutInitialize failed: 0x%x\n", rc);
		return qfalse;
	}

	ring = calloc(NX_RING_SAMPLES, sizeof(s16));
	chunk_mem = aligned_alloc(0x1000, NX_CHUNKS * NX_CHUNK_BYTES);
	if (!ring || !chunk_mem)
	{
		Com_Printf("Failed allocating memory for sound\n");
		FreeMemory();
		audoutExit();
		return qfalse;
	}

	memset(&dma, 0, sizeof(dma));
	dma.samplebits = 16;
	dma.speed = NX_MIX_RATE;
	dma.channels = NX_CHANNELS;
	dma.samples = NX_RING_SAMPLES;
	dma.fullsamples = NX_RING_SAMPLES / NX_CHANNELS;
	dma.submission_chunk = 1;
	dma.buffer = (byte *) ring;
	dma_pos = 0;
	last_left = last_right = 0;

	rc = audoutStartAudioOut();
	if (R_FAILED(rc))
	{
		Com_Printf("audoutStartAudioOut failed: 0x%x\n", rc);
		goto fail;
	}

	for (i = 0; i < NX_CHUNKS; i++)
	{
		chunks[i].next = NULL;
		chunks[i].buffer = (u8 *) chunk_mem + i * NX_CHUNK_BYTES;
		chunks[i].buffer_size = NX_CHUNK_BYTES;
		chunks[i].data_size = NX_CHUNK_BYTES;
		chunks[i].data_offset = 0;
		FillChunk(&chunks[i]);
		audoutAppendAudioOutBuffer(&chunks[i]);
	}

	snd_running = true;
	rc = threadCreate(&snd_thread, SoundThread, NULL, NULL, NX_THREAD_STACK, NX_THREAD_PRIO, NX_THREAD_CORE);
	if (R_SUCCEEDED(rc))
		rc = threadStart(&snd_thread);
	if (R_FAILED(rc))
	{
		Com_Printf("Failed starting the sound thread: 0x%x\n", rc);
		audoutStopAudioOut();
		goto fail;
	}

	Com_Printf("NX audout: mixing at %d Hz, playing at %d Hz\n", NX_MIX_RATE, NX_OUT_RATE);
	return qtrue;

fail:
	FreeMemory();
	audoutExit();
	return qfalse;
}

int SNDDMA_GetDMAPos (void)
{
	return dma_pos;
}

void SNDDMA_Shutdown (void)
{
	if (!ring)
		return;

	Com_Printf("Shutting down sound\n");
	snd_running = false;
	threadWaitForExit(&snd_thread);
	threadClose(&snd_thread);
	audoutStopAudioOut();
	audoutExit();
	dma.buffer = NULL;
	FreeMemory();
}

void SNDDMA_BeginPainting (void)
{
}

void SNDDMA_Submit (void)
{
	/* The ring is read by the CPU only: nothing to hand over. */
}
