/* nx_crash.c -- writes a crash report to SD_LOG when the process faults.
 *
 * Addresses are also given relative to the start of the program, so that
 * they can be fed to aarch64-none-elf-addr2line along with the ELF file:
 *
 *	aarch64-none-elf-addr2line -f -e RtCWNX.elf <offset>
 */

#include <stdalign.h>
#include <stdio.h>

#include <switch.h>

#include "nx_paths.h"
#include "nx_platform.h"

#define MAX_BACKTRACE	32

alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

extern char __start__[];	/* the program's load address, from libnx's crt0 */

static const char *ExceptionName (u32 desc)
{
	switch (desc)
	{
	case ThreadExceptionDesc_InstructionAbort:	return "instruction abort";
	case ThreadExceptionDesc_MisalignedPC:		return "misaligned PC";
	case ThreadExceptionDesc_MisalignedSP:		return "misaligned SP";
	case ThreadExceptionDesc_SError:		return "SError";
	case ThreadExceptionDesc_BadSVC:		return "bad SVC";
	case ThreadExceptionDesc_Trap:			return "trap";
	case ThreadExceptionDesc_Other:			return "data abort or other";
	default:					return "unknown";
	}
}

static bool IsReadable (u64 addr, u64 size)
{
	MemoryInfo	info;
	u32		page;

	if (addr & 7)
		return false;
	if (R_FAILED(svcQueryMemory(&info, &page, addr)))
		return false;
	return (info.perm & Perm_R) && addr + size <= info.addr + info.size;
}

static void Report (FILE *f, ThreadExceptionDump *ctx)
{
	const u64	base = (u64) __start__;
	u64		fp = ctx->fp.x;
	int		i;

	fprintf(f, "=== RtCWNX crash report ===\n");
	fprintf(f, "exception: %s (0x%x)\n", ExceptionName(ctx->error_desc), ctx->error_desc);
	fprintf(f, "base: 0x%016lx\n", base);
	fprintf(f, "pc  : 0x%016lx (offset 0x%lx)\n", ctx->pc.x, ctx->pc.x - base);
	fprintf(f, "lr  : 0x%016lx (offset 0x%lx)\n", ctx->lr.x, ctx->lr.x - base);
	fprintf(f, "sp  : 0x%016lx\n", ctx->sp.x);
	fprintf(f, "fp  : 0x%016lx\n", ctx->fp.x);
	fprintf(f, "far : 0x%016lx\n", ctx->far.x);
	fprintf(f, "esr : 0x%08x  pstate: 0x%08x\n", ctx->esr, ctx->pstate);

	for (i = 0; i < 29; i++)
		fprintf(f, "x%-2d : 0x%016lx%s", i, ctx->cpu_gprs[i].x, (i & 1) ? "\n" : "  ");
	fprintf(f, "\n");

	fprintf(f, "backtrace (offsets):\n");
	fprintf(f, "  0x%lx\n  0x%lx\n", ctx->pc.x - base, ctx->lr.x - base);
	for (i = 0; i < MAX_BACKTRACE && IsReadable(fp, 16); i++)
	{
		const u64	*frame = (const u64 *) fp;

		if (frame[1] == 0)
			break;
		fprintf(f, "  0x%lx\n", frame[1] - base);
		if (frame[0] <= fp)
			break;
		fp = frame[0];
	}
}

void __libnx_exception_handler (ThreadExceptionDump *ctx)
{
	FILE	*f;

	NX_LogPrintf("\nFATAL: %s at offset 0x%lx, see " NX_CRASH_FILE "\n",
			ExceptionName(ctx->error_desc), ctx->pc.x - (u64) __start__);
	NX_LogFlush();

	f = fopen(NX_CRASH_FILE, "w");
	if (f)
	{
		Report(f, ctx);
		fclose(f);
	}
}
