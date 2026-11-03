/* sys_nx.c -- the system interface on Horizon, in place of iortcw's sys_unix.c. */

#include <dirent.h>
#include <errno.h>
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "sys/sys_local.h"

#include "nx_paths.h"
#include "nx_platform.h"

/* A pk3's central directory is read a few bytes at a time, and a card read is
 * slow however little it asks for: this is what makes the start bearable. */
#define FILE_BUFFER	(64 * 1024)

#define MAX_FOUND_FILES	0x1000

qboolean stdinIsATTY = qfalse;

static u64	clock_base;
static s64	clock_skipped_ms;

/*
===============================================================================

PATHS

===============================================================================
*/

char *Sys_DefaultHomePath (void)
{
	return (char *) NX_GAME_DIR;
}

char *Sys_SteamPath (void)
{
	return (char *) "";
}

char *Sys_GogPath (void)
{
	return (char *) "";
}

char *Sys_Cwd (void)
{
	return (char *) NX_GAME_DIR;
}

const char *Sys_Basename (char *path)
{
	const char	*slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

const char *Sys_Dirname (char *path)
{
	static char	dir[MAX_OSPATH];
	char		*slash;

	Q_strncpyz(dir, path, sizeof(dir));
	slash = strrchr(dir, '/');
	if (slash)
		*slash = 0;
	else
		Q_strncpyz(dir, NX_GAME_DIR, sizeof(dir));
	return dir;
}

/*
===============================================================================

TIME AND MEMORY

===============================================================================
*/

int Sys_Milliseconds (void)
{
	const u64	ticks = armGetSystemTick();

	if (!clock_base)
		clock_base = ticks;
	return (int) ((s64) (armTicksToNs(ticks - clock_base) / 1000000) - clock_skipped_ms);
}

void NX_SkipClock (int msec)
{
	clock_skipped_ms += msec;
}

qboolean Sys_RandomBytes (byte *string, int len)
{
	randomGet(string, len);
	return qtrue;
}

char *Sys_GetCurrentUser (void)
{
	return (char *) "player";
}

qboolean Sys_LowPhysicalMemory (void)
{
	return qfalse;
}

void Sys_Sleep (int msec)
{
	if (msec == 0)
		return;
	svcSleepThread((s64) (msec < 0 ? 1 : msec) * 1000000);
}

void Sys_SetFloatEnv (void)
{
	fesetround(FE_TONEAREST);
}

void Sys_SetEnv (const char *name, const char *value)
{
	if (value && *value)
		setenv(name, value, 1);
	else
		unsetenv(name);
}

int Sys_PID (void)
{
	return 1;
}

qboolean Sys_PIDIsRunning (int pid)
{
	(void) pid;
	return qfalse;
}

int Sys_GetHighQualityCPU (void)
{
	return 1;
}

/*
===============================================================================

FILES

===============================================================================
*/

FILE *Sys_FOpen (const char *ospath, const char *mode)
{
	struct stat	buf;
	FILE		*f;

	/* make sure it isn't a directory */
	if (stat(ospath, &buf) == 0 && S_ISDIR(buf.st_mode))
		return NULL;

	f = fopen(ospath, mode);
	if (f)
		setvbuf(f, NULL, _IOFBF, FILE_BUFFER);
	return f;
}

qboolean Sys_Mkdir (const char *path)
{
	return (mkdir(path, 0777) == 0 || errno == EEXIST) ? qtrue : qfalse;
}

FILE *Sys_Mkfifo (const char *ospath)
{
	(void) ospath;
	return NULL;
}

/* The engine hides its CD key file from other users, of which there are none. */
mode_t umask (mode_t mask)
{
	return mask;
}

void Sys_Chmod (char *file, int mode)
{
	(void) file;
	(void) mode;
}

static void ListFilteredFiles (const char *basedir, char *subdirs, char *filter, char **list, int *numfiles)
{
	char		search[MAX_OSPATH], newsubdirs[MAX_OSPATH];
	char		filename[MAX_OSPATH];
	DIR		*fdir;
	struct dirent	*d;
	struct stat	st;

	if (*numfiles >= MAX_FOUND_FILES - 1)
		return;

	if (strlen(subdirs))
		Com_sprintf(search, sizeof(search), "%s/%s", basedir, subdirs);
	else
		Com_sprintf(search, sizeof(search), "%s", basedir);

	fdir = opendir(search);
	if (!fdir)
		return;

	while ((d = readdir(fdir)) != NULL)
	{
		Com_sprintf(filename, sizeof(filename), "%s/%s", search, d->d_name);
		if (stat(filename, &st) == -1)
			continue;

		if (S_ISDIR(st.st_mode) && Q_stricmp(d->d_name, ".") && Q_stricmp(d->d_name, ".."))
		{
			if (strlen(subdirs))
				Com_sprintf(newsubdirs, sizeof(newsubdirs), "%s/%s", subdirs, d->d_name);
			else
				Com_sprintf(newsubdirs, sizeof(newsubdirs), "%s", d->d_name);
			ListFilteredFiles(basedir, newsubdirs, filter, list, numfiles);
		}

		if (*numfiles >= MAX_FOUND_FILES - 1)
			break;

		Com_sprintf(filename, sizeof(filename), "%s/%s", subdirs, d->d_name);
		if (!Com_FilterPath(filter, filename, qfalse))
			continue;
		list[*numfiles] = CopyString(filename);
		(*numfiles)++;
	}
	closedir(fdir);
}

char **Sys_ListFiles (const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs)
{
	char		*list[MAX_FOUND_FILES];
	char		**listCopy;
	char		search[MAX_OSPATH];
	DIR		*fdir;
	struct dirent	*d;
	struct stat	st;
	qboolean	dironly = wantsubs;
	int		nfiles = 0;
	int		extLen;
	int		i;

	if (filter)
	{
		ListFilteredFiles(directory, (char *) "", filter, list, &nfiles);
		list[nfiles] = NULL;
		*numfiles = nfiles;
		if (!nfiles)
			return NULL;
		listCopy = Z_Malloc((nfiles + 1) * sizeof(*listCopy));
		for (i = 0; i < nfiles; i++)
			listCopy[i] = list[i];
		listCopy[i] = NULL;
		return listCopy;
	}

	if (!extension)
		extension = "";
	if (extension[0] == '/' && extension[1] == 0)
	{
		extension = "";
		dironly = qtrue;
	}
	extLen = strlen(extension);

	fdir = opendir(directory);
	if (!fdir)
	{
		*numfiles = 0;
		return NULL;
	}

	while ((d = readdir(fdir)) != NULL)
	{
		Com_sprintf(search, sizeof(search), "%s/%s", directory, d->d_name);
		if (stat(search, &st) == -1)
			continue;
		if ((dironly && !S_ISDIR(st.st_mode)) || (!dironly && S_ISDIR(st.st_mode)))
			continue;

		if (*extension)
		{
			const int nameLen = strlen(d->d_name);

			if (nameLen < extLen || Q_stricmp(d->d_name + nameLen - extLen, extension))
				continue;
		}

		if (nfiles == MAX_FOUND_FILES - 1)
			break;
		list[nfiles++] = CopyString(d->d_name);
	}
	list[nfiles] = NULL;
	closedir(fdir);

	*numfiles = nfiles;
	if (!nfiles)
		return NULL;

	listCopy = Z_Malloc((nfiles + 1) * sizeof(*listCopy));
	for (i = 0; i < nfiles; i++)
		listCopy[i] = list[i];
	listCopy[i] = NULL;
	return listCopy;
}

void Sys_FreeFileList (char **list)
{
	int	i;

	if (!list)
		return;
	for (i = 0; list[i]; i++)
		Z_Free(list[i]);
	Z_Free(list);
}

/*
===============================================================================

CONSOLE: the engine's output goes to the log

===============================================================================
*/

void CON_Shutdown (void)
{
}

void CON_Init (void)
{
}

char *CON_Input (void)
{
	return NULL;
}

void CON_Print (const char *msg)
{
	NX_LogWrite(msg);
}

/*
===============================================================================

DIALOGS AND OTHER PROGRAMS

===============================================================================
*/

/* What a player sees when the game cannot go on: the system's own error screen. */
void NX_FatalDialog (const char *message)
{
#ifndef NX_HOSTCHECK
	ErrorApplicationConfig	config;

	if (R_SUCCEEDED(errorApplicationCreate(&config, "Return to Castle Wolfenstein", message)))
		errorApplicationShow(&config);
#else
	(void) message;
#endif
}

void Sys_ErrorDialog (const char *error)
{
	FILE	*f;

	NX_LogPrintf("ERROR: %s\n", error);
	NX_LogFlush();

	f = fopen(NX_FATAL_FILE, "w");
	if (f)
	{
		fprintf(f, "%s\n", error);
		fclose(f);
	}
	NX_FatalDialog(error);
}

dialogResult_t Sys_Dialog (dialogType_t type, const char *message, const char *title)
{
	(void) type;
	NX_LogPrintf("%s: %s\n", title, message);
	return DR_NO;
}

void Sys_GLimpSafeInit (void)
{
}

void Sys_GLimpInit (void)
{
}

void Sys_PlatformInit (void)
{
	Sys_SetFloatEnv();
}

void Sys_PlatformExit (void)
{
}

qboolean Sys_DllExtension (const char *name)
{
	(void) name;
	return qfalse;
}

char *Sys_GetDLLName (const char *name)
{
	return va("%s" DLL_EXT, name);
}

/* The menus' "Multiplayer" and "Single Player" entries start the other game. */
void Sys_StartProcess (char *cmdline, qboolean doexit)
{
	const char	*target;

	if (Q_stristr(cmdline, "iowolfmp"))
		target = NX_MP_NRO;
	else if (Q_stristr(cmdline, "iowolfsp"))
		target = NX_SP_NRO;
	else
	{
		Com_Printf("Not starting %s\n", cmdline);
		return;
	}

	if (access(target, F_OK) != 0)
	{
		Com_Printf(S_COLOR_YELLOW "WARNING: %s is missing\n", target);
		return;
	}

	Com_Printf("Starting %s\n", target);
	envSetNextLoad(target, target);
	if (doexit)
		Cbuf_ExecuteText(EXEC_APPEND, "quit\n");
}

#ifdef RTCW_MP
void Sys_OpenURL (const char *url, qboolean doexit)
#else
void Sys_OpenURL (char *url, qboolean doexit)
#endif
{
	(void) doexit;
	Com_Printf("Not opening %s\n", url);
}
