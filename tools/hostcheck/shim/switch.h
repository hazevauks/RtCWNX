/* switch.h -- just enough of libnx for the port's own code to run on a PC.
 *
 * With this header in place of the real one, source/platform/ and the engine
 * compile unchanged for the machine that builds them, which gives a way to run
 * the game on its real data with no console around. See tools/hostcheck/README.md.
 */

#ifndef HOSTCHECK_SWITCH_H
#define HOSTCHECK_SWITCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <arpa/inet.h>

typedef uint8_t		u8;
typedef uint16_t	u16;
typedef uint32_t	u32;
typedef uint64_t	u64;
typedef int16_t		s16;
typedef int32_t		s32;
typedef int64_t		s64;
typedef u32		Result;

#define R_SUCCEEDED(rc)	((rc) == 0)
#define R_FAILED(rc)	((rc) != 0)

/* time, virtual: one more frame of a 60 Hz console per main loop */
u64 armGetSystemTick (void);
u64 armGetSystemTickFreq (void);
u64 armTicksToNs (u64 ticks);
void svcSleepThread (s64 ns);

/* process */
#define CUR_PROCESS_HANDLE	0
enum { InfoType_TotalMemorySize = 6, InfoType_UsedMemorySize = 7 };
Result svcGetInfo (u64 *out, u32 type, u32 handle, u64 sub);
Result socketInitializeDefault (void);
void socketExit (void);
extern struct in_addr __nxlink_host;
int nxlinkStdio (void);
Result romfsInit (void);
Result romfsExit (void);
Result envSetNextLoad (const char *path, const char *argv);
void randomGet (void *buf, size_t len);

/* applet */
enum { AppletType_Application = 0, AppletType_SystemApplication = 1 };
bool appletMainLoop (void);
int appletGetAppletType (void);

/* locks */
typedef pthread_mutex_t Mutex;
#define mutexInit(m)	pthread_mutex_init(m, NULL)
#define mutexLock(m)	pthread_mutex_lock(m)
#define mutexUnlock(m)	pthread_mutex_unlock(m)

/* threads */
typedef void (*ThreadFunc) (void *);
typedef struct { void *impl; ThreadFunc entry; void *arg; size_t stack; } Thread;
Result threadCreate (Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid);
Result threadStart (Thread *t);
Result threadWaitForExit (Thread *t);
Result threadClose (Thread *t);
void threadExit (void);
void armDCacheFlush (void *addr, size_t size);

/* controller */
#define HOST_BIT(n)	(1ULL << (n))
enum
{
	HidNpadButton_A = HOST_BIT(0),
	HidNpadButton_B = HOST_BIT(1),
	HidNpadButton_X = HOST_BIT(2),
	HidNpadButton_Y = HOST_BIT(3),
	HidNpadButton_StickL = HOST_BIT(4),
	HidNpadButton_StickR = HOST_BIT(5),
	HidNpadButton_L = HOST_BIT(6),
	HidNpadButton_R = HOST_BIT(7),
	HidNpadButton_ZL = HOST_BIT(8),
	HidNpadButton_ZR = HOST_BIT(9),
	HidNpadButton_Plus = HOST_BIT(10),
	HidNpadButton_Minus = HOST_BIT(11),
	HidNpadButton_Left = HOST_BIT(12),
	HidNpadButton_Up = HOST_BIT(13),
	HidNpadButton_Right = HOST_BIT(14),
	HidNpadButton_Down = HOST_BIT(15)
};
#define HidNpadStyleSet_NpadStandard	0
typedef struct { int unused; } PadState;
typedef struct { s32 x, y; } HidAnalogStickState;
void padConfigureInput (u32 max_players, u32 styles);
void padInitializeDefault (PadState *pad);
void padUpdate (PadState *pad);
u64 padGetButtons (const PadState *pad);
HidAnalogStickState padGetStickPos (const PadState *pad, u32 id);

/* touch screen */
typedef struct { u32 x, y; } HidTouchState;
typedef struct { s32 count; HidTouchState touches[16]; } HidTouchScreenState;
void hidInitializeTouchScreen (void);
size_t hidGetTouchScreenStates (HidTouchScreenState *states, size_t count);

/* system keyboard: a scenario answers it */
typedef struct { int unused; } SwkbdConfig;
Result swkbdCreate (SwkbdConfig *config, s32 max_dictwords);
void swkbdConfigMakePresetDefault (SwkbdConfig *config);
void swkbdConfigSetHeaderText (SwkbdConfig *config, const char *text);
Result swkbdShow (SwkbdConfig *config, char *out, size_t out_size);
void swkbdClose (SwkbdConfig *config);

/* audout */
typedef struct AudioOutBuffer AudioOutBuffer;
struct AudioOutBuffer
{
	AudioOutBuffer	*next;
	void		*buffer;
	u64		buffer_size;
	u64		data_size;
	u64		data_offset;
};
Result audoutInitialize (void);
void audoutExit (void);
Result audoutStartAudioOut (void);
Result audoutStopAudioOut (void);
Result audoutAppendAudioOutBuffer (AudioOutBuffer *buffer);
Result audoutGetReleasedAudioOutBuffer (AudioOutBuffer **buffer, u32 *count);
Result audoutWaitPlayFinish (AudioOutBuffer **released, u32 *count, u64 timeout);

#endif	/* HOSTCHECK_SWITCH_H */
