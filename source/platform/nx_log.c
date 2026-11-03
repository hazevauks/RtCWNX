/* nx_log.c -- log file on the SD card, and nxlink when connected.
 *
 * Everything goes to SD_LOG/rtcw_log.txt, stdout and stderr too. The file is
 * flushed at most four times a second, and on warnings and errors, so the last
 * lines survive a crash without paying for a card write per line during loading.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <switch.h>

#include "nx_paths.h"
#include "nx_platform.h"

#define LOG_BUFFER	(32 * 1024)

static FILE	*log_file;
static int	nxlink_fd = -1;
static Mutex	log_mutex;
static u64	last_flush;

void NX_LogInit (void)
{
	time_t		now = time(NULL);
	const struct tm	*t = localtime(&now);

	mkdir(NX_GAME_DIR, 0777);
	mkdir(NX_LOG_DIR, 0777);
	mutexInit(&log_mutex);

	/* Keep the previous run: it is the one that crashed. */
	remove(NX_LOG_FILE_OLD);
	rename(NX_LOG_FILE, NX_LOG_FILE_OLD);

	log_file = fopen(NX_LOG_FILE, "w");
	if (log_file)
		setvbuf(log_file, NULL, _IOFBF, LOG_BUFFER);

	if (__nxlink_host.s_addr != 0)
		nxlink_fd = nxlinkStdio();

	/* Without nxlink, what libraries print goes to the log file too, through
	 * its descriptor: the SD card's file system opens a file to write once. */
	if (nxlink_fd < 0 && log_file)
	{
		dup2(fileno(log_file), STDOUT_FILENO);
		dup2(fileno(log_file), STDERR_FILENO);
	}

	NX_LogPrintf("RtCWNX log, %04d-%02d-%02d %02d:%02d:%02d, nxlink %s\n",
			t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
			t->tm_hour, t->tm_min, t->tm_sec,
			(nxlink_fd >= 0) ? "on" : "off");
}

void NX_LogShutdown (void)
{
	if (log_file)
	{
		fclose(log_file);
		log_file = NULL;
	}
	if (nxlink_fd >= 0)
	{
		close(nxlink_fd);
		nxlink_fd = -1;
	}
}

void NX_LogWrite (const char *text)
{
	mutexLock(&log_mutex);
	if (log_file)
	{
		const u64	now = armGetSystemTick();

		fputs(text, log_file);
		if (strstr(text, "WARNING") || strstr(text, "ERROR")
				|| now - last_flush > armGetSystemTickFreq() / 4)
		{
			fflush(log_file);
			last_flush = now;
		}
	}
	if (nxlink_fd >= 0)
		fputs(text, stdout);
	mutexUnlock(&log_mutex);
}

void NX_LogPrintf (const char *fmt, ...)
{
	char	buf[1024];
	va_list	argptr;

	va_start(argptr, fmt);
	vsnprintf(buf, sizeof(buf), fmt, argptr);
	va_end(argptr);
	NX_LogWrite(buf);
}

void NX_LogFlush (void)
{
	if (log_file)
		fflush(log_file);
}

void NX_LogMemory (const char *when)
{
	u64	total = 0, used = 0;

	svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
	NX_LogPrintf("memory (%s): %lu MB used of %lu MB\n", when,
			(unsigned long) (used >> 20), (unsigned long) (total >> 20));
}
