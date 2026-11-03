/* nx_modules.c -- cgame, qagame and ui as parts of the executable.
 *
 * Homebrew cannot load a library, but the game's modules are written to be one:
 * they repeat each other's global names (cmake/RtcwModule.cmake keeps those
 * private), and each map or menu change unloads and loads them again, which on a
 * PC starts them with fresh globals. Some of their code counts on it, with handles
 * and pointers it would not otherwise reset. So a load here does the same: .data
 * comes back from a copy made before the module first ran, and .bss is cleared.
 */

#include <stdlib.h>
#include <string.h>

#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"

#include "nx_platform.h"

typedef void (*dllEntry_t) (intptr_t (*syscallptr)(intptr_t, ...));

typedef struct
{
	const char	*name;
	dllEntry_t	dllEntry;
	vmMainProc	vmMain;
	char		*data_start, *data_end;
	char		*bss_start, *bss_end;
	void		*data_copy;
} nxModule_t;

#define MODULE_EXTERNS(m) \
	extern void rtcw_##m##_dllEntry (intptr_t (*syscallptr)(intptr_t, ...)); \
	extern intptr_t rtcw_##m##_vmMain (int command, int arg0, int arg1, int arg2, int arg3, int arg4, int arg5, \
			int arg6, int arg7, int arg8, int arg9, int arg10, int arg11); \
	extern char rtcw_##m##_data_start[], rtcw_##m##_data_end[], rtcw_##m##_bss_start[], rtcw_##m##_bss_end[]

#define MODULE_ENTRY(m) \
	{ #m, rtcw_##m##_dllEntry, (vmMainProc) rtcw_##m##_vmMain, \
	  rtcw_##m##_data_start, rtcw_##m##_data_end, rtcw_##m##_bss_start, rtcw_##m##_bss_end, NULL }

MODULE_EXTERNS(cgame);
MODULE_EXTERNS(qagame);
MODULE_EXTERNS(ui);

static nxModule_t modules[] =
{
	MODULE_ENTRY(cgame),
	MODULE_ENTRY(qagame),
	MODULE_ENTRY(ui),
};

void *NX_LoadModule (const char *name, void *entryPoint, intptr_t (*systemCalls)(intptr_t, ...))
{
	size_t	i;

	for (i = 0; i < ARRAY_LEN(modules); i++)
	{
		nxModule_t	*m = &modules[i];
		const size_t	data_size = m->data_end - m->data_start;

		if (Q_stricmp(name, m->name) != 0)
			continue;

		if (!m->data_copy)
		{
			m->data_copy = malloc(data_size ? data_size : 1);
			if (!m->data_copy)
				Com_Error(ERR_FATAL, "NX_LoadModule: out of memory for %s", name);
			memcpy(m->data_copy, m->data_start, data_size);
		}
		else
		{
			memcpy(m->data_start, m->data_copy, data_size);
		}
		memset(m->bss_start, 0, m->bss_end - m->bss_start);

		Com_DPrintf("Module %s: %u KB of data, %u KB of bss\n", name,
				(unsigned) (data_size / 1024), (unsigned) ((m->bss_end - m->bss_start) / 1024));

		*(vmMainProc *) entryPoint = m->vmMain;
		m->dllEntry(systemCalls);
		return m;
	}
	return NULL;
}
