/* glimp_dk.c -- what the renderer asks of its system, with no OpenGL driver around. */

#include "renderer/tr_local.h"

#include "dk_device.h"
#include "dkgl.h"
#include "nx_platform.h"

int qglMajorVersion, qglMinorVersion;
int qglesMajorVersion, qglesMinorVersion;

void (APIENTRYP qglActiveTextureARB) (GLenum texture);
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture);
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t);
void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count);
void (APIENTRYP qglUnlockArraysEXT) (void);

#define GLE(ret, name, ...) name##proc * qgl##name = NULL;
QGL_1_1_PROCS;
QGL_1_1_FIXED_FUNCTION_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
QGL_ES_1_1_PROCS;
QGL_ES_1_1_FIXED_FUNCTION_PROCS;
QGL_1_3_PROCS;
QGL_1_5_PROCS;
QGL_2_0_PROCS;
QGL_3_0_PROCS;
QGL_ARB_occlusion_query_PROCS;
QGL_ARB_framebuffer_object_PROCS;
QGL_ARB_vertex_array_object_PROCS;
QGL_EXT_direct_state_access_PROCS;
#undef GLE

void GLimp_Init (qboolean fixedFunction)
{
	int	width, height;

	(void) fixedFunction;
	ri.Sys_GLimpInit();

	if (!DEV_Init(&width, &height))
		ri.Error(ERR_FATAL, "GLimp_Init: could not start deko3D");

	glConfig.vidWidth = width;
	glConfig.vidHeight = height;
	glConfig.windowAspect = (float) width / height;
	glConfig.isFullscreen = qtrue;
	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;
	glConfig.displayFrequency = 60;
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = qtrue;
	glConfig.textureCompression = TC_NONE;
	glConfig.textureEnvAddAvailable = qtrue;
	glConfig.numTextureUnits = DEV_MAX_TEXTURE_UNITS;

	DKGL_Init();
	sscanf((const char *) qglGetString(GL_VERSION), "%d.%d", &qglMajorVersion, &qglMinorVersion);

	Q_strncpyz(glConfig.vendor_string, (char *) qglGetString(GL_VENDOR), sizeof(glConfig.vendor_string));
	Q_strncpyz(glConfig.renderer_string, (char *) qglGetString(GL_RENDERER), sizeof(glConfig.renderer_string));
	Q_strncpyz(glConfig.version_string, (char *) qglGetString(GL_VERSION), sizeof(glConfig.version_string));
	Q_strncpyz(glConfig.extensions_string, (char *) qglGetString(GL_EXTENSIONS), sizeof(glConfig.extensions_string));

	NX_LogPrintf("deko3D: %dx%d\n", width, height);

	ri.Cvar_Get("r_availableModes", "", CVAR_ROM);
	ri.IN_Init(NULL);
}

void GLimp_Shutdown (void)
{
	ri.IN_Shutdown();
	DKGL_Shutdown();
	DEV_Shutdown();
}

void GLimp_EndFrame (void)
{
	DEV_Present();
}

void GLimp_Minimize (void)
{
}

void GLimp_LogComment (char *comment)
{
	(void) comment;
}

void GLimp_SetGamma (unsigned char red[256], unsigned char green[256], unsigned char blue[256])
{
	DEV_SetGammaTable(red, green, blue);
}
