/* host_main.c -- entry point of the host check: the console's main(), renamed by the
 * build, after a scenario was loaded for the shim to follow.
 *
 *	rtcw_host <scenario file> <output folder> [engine arguments]
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

void Shim_LoadScenario (const char *path, const char *output_dir);
int NX_Main (int argc, char **argv);

/* -fsanitize=alignment traps with an illegal instruction: this says where. */
static void OnFault (int sig, siginfo_t *info, void *context)
{
	(void) context;
	fprintf(stderr, "signal %d at %p\n", sig, info->si_addr);
	fflush(stderr);
	_Exit(128 + sig);
}

int main (int argc, char **argv)
{
	struct sigaction	action;

	if (argc < 3)
	{
		fprintf(stderr, "usage: %s <scenario> <output folder> [engine arguments]\n", argv[0]);
		return 2;
	}

	action.sa_sigaction = OnFault;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGFPE, &action, NULL);

	Shim_LoadScenario(argv[1], argv[2]);

	/* The engine's arguments, with what stands for the program's path first. */
	argv[2] = argv[0];
	return NX_Main(argc - 2, argv + 2);
}
