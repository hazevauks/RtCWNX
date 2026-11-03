/* switch_shim.c -- runs the port on a PC, from a scenario instead of a player.
 *
 * Time is virtual: the main loop is a 60 Hz console's, however fast the PC is,
 * so a scenario gets the same frames on every run. A scenario file has a line
 * per event, by frame number:
 *
 *	120 press A
 *	125 release A
 *	300 stick L 0 32767	(the stick's x and y)
 *	900 shot name		(this frame is drawn, to name.png in the output folder)
 *	950 cmd map escape1	(a console command)
 *	960 touch 640 360	(a finger down, in screen pixels; "touch -1 -1" lifts it)
 *	970 text hello		(what the system keyboard will answer)
 *	980 stats name		(what this frame drew, in the log)
 *	2000 quit
 *
 * A frame number starting with + counts from the event before it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

#include "host_device.h"

#define NS_PER_FRAME		16666667ULL
#define NS_PER_CLOCK_READ	1000ULL
#define CHUNK_QUEUE		8
#define MAX_EVENTS		8192

typedef struct
{
	int	frame;
	char	action[16];
	char	arg[64];
	char	line[192];	/* what a cmd event gives the console */
	int	x, y;
} event_t;

void Cbuf_AddText (const char *text);

static event_t	events[MAX_EVENTS];
static int	event_count, event_next;
static int	frame_number;
static bool	quit_requested;

static volatile u64	virtual_ns;

static u64			buttons;
static HidAnalogStickState	sticks[2];
static int			touch_x = -1, touch_y = -1;
static char			keyboard_text[192];

static const char	*out_dir = ".";

/* ---- scenario ---- */

void Shim_LoadScenario (const char *path, const char *output_dir)
{
	char	line[256];
	FILE	*f = fopen(path, "r");
	int	previous = 0;

	out_dir = output_dir;
	if (!f)
	{
		fprintf(stderr, "no scenario: %s\n", path);
		exit(2);
	}
	while (event_count < MAX_EVENTS && fgets(line, sizeof(line), f))
	{
		event_t		*e = &events[event_count];
		char		when[16];
		int		fields;

		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		e->arg[0] = 0;
		fields = sscanf(line, "%15s %15s %63s %d %d", when, e->action, e->arg, &e->x, &e->y);
		if (fields >= 2)
		{
			const char	*rest = strstr(line, e->action) + strlen(e->action);

			e->frame = (when[0] == '+') ? previous + atoi(when + 1) : atoi(when);
			previous = e->frame;
			snprintf(e->line, sizeof(e->line), "%s", rest);
			event_count++;
		}
	}
	fclose(f);
}

const char *Shim_OutputDir (void)
{
	return out_dir;
}

static u64 ButtonMask (const char *name)
{
	static const struct { const char *name; u64 mask; } table[] =
	{
		{ "A", HidNpadButton_A }, { "B", HidNpadButton_B },
		{ "X", HidNpadButton_X }, { "Y", HidNpadButton_Y },
		{ "L", HidNpadButton_L }, { "R", HidNpadButton_R },
		{ "ZL", HidNpadButton_ZL }, { "ZR", HidNpadButton_ZR },
		{ "PLUS", HidNpadButton_Plus }, { "MINUS", HidNpadButton_Minus },
		{ "L3", HidNpadButton_StickL }, { "R3", HidNpadButton_StickR },
		{ "UP", HidNpadButton_Up }, { "DOWN", HidNpadButton_Down },
		{ "LEFT", HidNpadButton_Left }, { "RIGHT", HidNpadButton_Right }
	};
	size_t	i;

	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
	{
		if (!strcmp(table[i].name, name))
			return table[i].mask;
	}
	fprintf(stderr, "scenario: no button %s\n", name);
	exit(2);
}

static void RunEvents (void)
{
	while (event_next < event_count && events[event_next].frame <= frame_number)
	{
		const event_t	*e = &events[event_next++];

		if (!strcmp(e->action, "press"))
			buttons |= ButtonMask(e->arg);
		else if (!strcmp(e->action, "release"))
			buttons &= ~ButtonMask(e->arg);
		else if (!strcmp(e->action, "stick"))
		{
			HidAnalogStickState	*s = &sticks[e->arg[0] == 'R'];

			s->x = e->x;
			s->y = e->y;
		}
		else if (!strcmp(e->action, "shot"))
			Host_RequestShot(e->arg);
		else if (!strcmp(e->action, "stats"))
			Host_RequestStats(e->arg);
		else if (!strcmp(e->action, "cmd"))
			Cbuf_AddText(e->line);
		else if (!strcmp(e->action, "touch"))
		{
			touch_x = atoi(e->arg);
			touch_y = e->x;
		}
		else if (!strcmp(e->action, "text"))
		{
			snprintf(keyboard_text, sizeof(keyboard_text), "%s", e->arg);
		}
		else if (!strcmp(e->action, "quit"))
			quit_requested = true;
	}
}

/* ---- time ---- */

u64 armGetSystemTick (void)
{
	/* Reading the clock takes time, or waiting on it would never end. */
	virtual_ns += NS_PER_CLOCK_READ;
	return virtual_ns;
}

u64 armGetSystemTickFreq (void)
{
	return 1000000000ULL;
}

u64 armTicksToNs (u64 ticks)
{
	return ticks;
}

void svcSleepThread (s64 ns)
{
	virtual_ns += (u64) ns;
}

/* ---- process ---- */

struct in_addr __nxlink_host;

int nxlinkStdio (void)
{
	return -1;
}

Result svcGetInfo (u64 *out, u32 type, u32 handle, u64 sub)
{
	(void) type; (void) handle; (void) sub;
	*out = 0;
	return 0;
}

Result socketInitializeDefault (void)
{
	return 0;
}

void socketExit (void)
{
}

Result romfsInit (void)
{
	return 0;
}

Result romfsExit (void)
{
	return 0;
}

Result envSetNextLoad (const char *path, const char *argv)
{
	(void) argv;
	printf("next: %s\n", path);
	return 0;
}

void randomGet (void *buf, size_t len)
{
	memset(buf, 0x5a, len);
}

int appletGetAppletType (void)
{
	return AppletType_Application;
}

bool appletMainLoop (void)
{
	virtual_ns += NS_PER_FRAME;
	frame_number++;
	return !quit_requested;
}

/* ---- threads ---- */

static void *ThreadTrampoline (void *param)
{
	Thread	*t = (Thread *) param;

	t->entry(t->arg);
	return NULL;
}

Result threadCreate (Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid)
{
	(void) stack_mem; (void) prio; (void) cpuid;
	t->entry = entry;
	t->arg = arg;
	t->stack = stack_sz;
	t->impl = malloc(sizeof(pthread_t));
	return 0;
}

Result threadStart (Thread *t)
{
	pthread_attr_t	attr;
	int		rc;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, t->stack);
	rc = pthread_create((pthread_t *) t->impl, &attr, ThreadTrampoline, t);
	pthread_attr_destroy(&attr);
	return (Result) rc;
}

Result threadWaitForExit (Thread *t)
{
	return (Result) pthread_join(*(pthread_t *) t->impl, NULL);
}

Result threadClose (Thread *t)
{
	free(t->impl);
	t->impl = NULL;
	return 0;
}

void threadExit (void)
{
	pthread_exit(NULL);
}

void armDCacheFlush (void *addr, size_t size)
{
	(void) addr; (void) size;
}

/* ---- controller ---- */

void padConfigureInput (u32 max_players, u32 styles)
{
	(void) max_players; (void) styles;
}

void padInitializeDefault (PadState *pad)
{
	(void) pad;
}

void padUpdate (PadState *pad)
{
	(void) pad;
	RunEvents();
}

u64 padGetButtons (const PadState *pad)
{
	(void) pad;
	return buttons;
}

HidAnalogStickState padGetStickPos (const PadState *pad, u32 id)
{
	(void) pad;
	return sticks[id & 1];
}

void hidInitializeTouchScreen (void)
{
}

size_t hidGetTouchScreenStates (HidTouchScreenState *states, size_t count)
{
	(void) count;
	states->count = 0;
	if (touch_x >= 0)
	{
		states->count = 1;
		states->touches[0].x = touch_x;
		states->touches[0].y = touch_y;
	}
	return 1;
}

/* ---- system keyboard ---- */

Result swkbdCreate (SwkbdConfig *config, s32 max_dictwords)
{
	(void) config; (void) max_dictwords;
	return 0;
}

void swkbdConfigMakePresetDefault (SwkbdConfig *config)
{
	(void) config;
}

void swkbdConfigSetHeaderText (SwkbdConfig *config, const char *text)
{
	(void) config; (void) text;
}

Result swkbdShow (SwkbdConfig *config, char *out, size_t out_size)
{
	(void) config;
	snprintf(out, out_size, "%s", keyboard_text);
	return 0;
}

void swkbdClose (SwkbdConfig *config)
{
	(void) config;
}

/* ---- audout: plays to a raw file, 48 kHz, stereo, 16 bits ---- */

static AudioOutBuffer	*chunk_queue[CHUNK_QUEUE];
static int		chunk_head, chunk_count;
static u64		chunk_due_ns;
static FILE		*audio_file;
static pthread_mutex_t	chunk_lock = PTHREAD_MUTEX_INITIALIZER;

Result audoutInitialize (void)
{
	char	path[512];

	snprintf(path, sizeof(path), "%s/audio.raw", out_dir);
	audio_file = fopen(path, "wb");
	return 0;
}

void audoutExit (void)
{
	if (audio_file)
		fclose(audio_file);
	audio_file = NULL;
}

Result audoutStartAudioOut (void)
{
	chunk_due_ns = virtual_ns;
	return 0;
}

Result audoutStopAudioOut (void)
{
	return 0;
}

Result audoutAppendAudioOutBuffer (AudioOutBuffer *buffer)
{
	pthread_mutex_lock(&chunk_lock);
	chunk_queue[(chunk_head + chunk_count++) % CHUNK_QUEUE] = buffer;
	pthread_mutex_unlock(&chunk_lock);
	return 0;
}

Result audoutGetReleasedAudioOutBuffer (AudioOutBuffer **buffer, u32 *count)
{
	AudioOutBuffer	*done = NULL;

	pthread_mutex_lock(&chunk_lock);
	if (chunk_count > 0 && virtual_ns >= chunk_due_ns)
	{
		done = chunk_queue[chunk_head];
		chunk_head = (chunk_head + 1) % CHUNK_QUEUE;
		chunk_count--;
		chunk_due_ns += done->data_size * 1000000000ULL / (48000 * 4);
		if (audio_file)
			fwrite(done->buffer, 1, done->data_size, audio_file);
	}
	pthread_mutex_unlock(&chunk_lock);

	*buffer = done;
	*count = done ? 1 : 0;
	return 0;
}

Result audoutWaitPlayFinish (AudioOutBuffer **released, u32 *count, u64 timeout)
{
	(void) timeout;
	usleep(500);
	return audoutGetReleasedAudioOutBuffer(released, count);
}
