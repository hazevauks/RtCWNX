/* host_scenario.h -- what a scenario asks of soft_gpu.c. */

#ifndef HOST_SCENARIO_H
#define HOST_SCENARIO_H

void Host_RequestShot (const char *name);
void Host_RequestStats (const char *name);
const char *Shim_OutputDir (void);

#endif	/* HOST_SCENARIO_H */
