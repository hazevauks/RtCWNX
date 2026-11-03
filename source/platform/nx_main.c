/* nx_main.c -- the process: services, the log, and the engine's own thread.
 *
 * The engine runs on a thread of its own because the main thread's stack is the
 * loader's choice, and the game's recursive code expects what a PC gives it.
 * It leaves through NX_ExitEngine, and the process ends from main.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "nx_paths.h"
#include "nx_platform.h"

#define ENGINE_STACK_SIZE	(8 * 1024 * 1024)
#define ENGINE_THREAD_PRIO	0x2C
#define ENGINE_THREAD_CORE	0
#define MAX_ARGS		64
#define MAX_ARGS_FILE		4096

int Sys_Main (int argc, char **argv);

static Thread	engine_thread;
static int	engine_argc;
static char	*engine_argv[MAX_ARGS];
static char	args_text[MAX_ARGS_FILE];
static int	exit_code;

void NX_SetLoadingBoost (int enable)
{
	static int	boosted;

	if (boosted != enable)
	{
		/* a mode for loading screens: it takes from the GPU what it gives the CPU */
		appletSetCpuBoostMode(enable ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
		boosted = enable;
	}
}

void NX_ExitEngine (int code)
{
	exit_code = code;
	threadExit();
	__builtin_unreachable();
}

static void EngineThread (void *unused)
{
	(void) unused;
	exit_code = Sys_Main(engine_argc, engine_argv);
}

/* What the loader passed, then what sd:/switch/RtCWNX/args.txt adds. */
static void BuildArguments (int argc, char **argv)
{
	FILE	*f;
	size_t	length;
	char	*p;
	int	i;

	for (i = 0; i < argc && engine_argc < MAX_ARGS; i++)
		engine_argv[engine_argc++] = argv[i];
	if (engine_argc == 0)
		engine_argv[engine_argc++] = (char *) "RtCWNX";

	/* Started from the album, the process has about 400 MB. The rest of the time it has
	 * more than the game has any use for. */
	if (appletGetAppletType() != AppletType_Application && appletGetAppletType() != AppletType_SystemApplication)
	{
		static char	*applet_args[] =
		{
			"+set", "com_hunkMegs", "80", "+set", "com_zoneMegs", "24",
			"+set", "com_soundMegs", "8", "+set", "r_picmip", "1"
		};

		for (i = 0; i < (int) (sizeof(applet_args) / sizeof(applet_args[0])) && engine_argc < MAX_ARGS; i++)
			engine_argv[engine_argc++] = applet_args[i];
	}

	f = fopen(NX_ARGS_FILE, "r");
	if (!f)
		return;
	length = fread(args_text, 1, sizeof(args_text) - 1, f);
	fclose(f);
	args_text[length] = 0;

	for (p = args_text; *p && engine_argc < MAX_ARGS; )
	{
		while (*p && isspace((unsigned char) *p))
			p++;
		if (!*p)
			break;
		engine_argv[engine_argc++] = p;
		while (*p && !isspace((unsigned char) *p))
			p++;
		if (*p)
			*p++ = 0;
	}
}

int main (int argc, char **argv)
{
	Result	rc;

	socketInitializeDefault();
	NX_LogInit();
	NX_LogMemory("start");
	if (appletGetAppletType() != AppletType_Application && appletGetAppletType() != AppletType_SystemApplication)
		NX_LogPrintf("WARNING: running in applet mode, with little memory and lower settings: start through a game's title override\n");

	/* a forwarder may not say where the NRO is */
	if (R_FAILED(romfsInit()))
		romfsMountFromFsdev(NX_OWN_NRO, 0, "romfs");

	NX_SetLoadingBoost(1);
	BuildArguments(argc, argv);

	rc = threadCreate(&engine_thread, EngineThread, NULL, NULL, ENGINE_STACK_SIZE, ENGINE_THREAD_PRIO, ENGINE_THREAD_CORE);
	if (R_SUCCEEDED(rc))
		rc = threadStart(&engine_thread);
	if (R_SUCCEEDED(rc))
	{
		threadWaitForExit(&engine_thread);
		threadClose(&engine_thread);
	}
	else
	{
		NX_LogPrintf("Failed starting the engine's thread: 0x%x\n", rc);
		exit_code = 1;
	}

	NX_SetLoadingBoost(0);
	romfsExit();
	NX_LogShutdown();
	socketExit();
	return exit_code;
}
