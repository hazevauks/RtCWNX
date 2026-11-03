/* nx_platform.h -- what the engine's patched files need from the port. */

#ifndef NX_PLATFORM_H
#define NX_PLATFORM_H

#include <stdint.h>

/* nx_main.c: ends the engine's thread, and main takes the process out from there */
void NX_ExitEngine (int code) __attribute__((noreturn));

/* nx_main.c: the CPU at its top speed through a load, and at its usual one in play */
void NX_SetLoadingBoost (int enable);

/* nx_log.c */
void NX_LogInit (void);
void NX_LogShutdown (void);
void NX_LogWrite (const char *text);
void NX_LogPrintf (const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void NX_LogFlush (void);
void NX_LogMemory (const char *when);

/* nx_modules.c: cgame, qagame and ui, as if each were a library loaded just now */
void *NX_LoadModule (const char *name, void *entryPoint, intptr_t (*systemCalls)(intptr_t, ...));

/* sys_nx.c: the clock, without the time the console spent asleep */
void NX_SkipClock (int msec);

/* in_nx.c, for the renderer's first frame in a mode */
void NX_FatalDialog (const char *message);

#endif	/* NX_PLATFORM_H */
