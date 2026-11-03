/* host_device.h -- what a scenario asks of the software device. */

#ifndef HOST_DEVICE_H
#define HOST_DEVICE_H

void Host_RequestShot (const char *name);
void Host_RequestStats (const char *name);
const char *Shim_OutputDir (void);

#endif	/* HOST_DEVICE_H */
