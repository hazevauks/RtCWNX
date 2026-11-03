/* dkgl.h -- the OpenGL layer's own interface: the renderer calls it through qgl*. */

#ifndef DKGL_H
#define DKGL_H

void DKGL_Init (void);
void DKGL_Shutdown (void);

/* In place of reading back the depth under a flare. Eye z is negative in front of the view. */
qboolean DKGL_FlareVisible (int id, int window_x, int window_y, float eye_z);

#endif	/* DKGL_H */
