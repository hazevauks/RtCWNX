/* glimp_gl.c -- the fallback renderer's window: OpenGL through Mesa and EGL.
 *
 * This is iortcw's own OpenGL 1 renderer on the driver every other Switch port
 * of an old game uses. It tells a problem of the platform from a problem of the
 * native renderer, which replaces this file.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <switch.h>

#include "renderer/tr_local.h"

#include "nx_platform.h"

#define SCREEN_WIDTH	1280
#define SCREEN_HEIGHT	720

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

static EGLDisplay	s_display = EGL_NO_DISPLAY;
static EGLContext	s_context = EGL_NO_CONTEXT;
static EGLSurface	s_surface = EGL_NO_SURFACE;

static void GetProcAddresses (void)
{
#define GLE(ret, name, ...) qgl##name = (name##proc *) eglGetProcAddress("gl" #name); \
	if (!qgl##name) ri.Printf(PRINT_ALL, "ERROR: Missing OpenGL function gl" #name "\n");

	QGL_1_1_PROCS;
	QGL_1_1_FIXED_FUNCTION_PROCS;
	QGL_DESKTOP_1_1_PROCS;
	QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
#undef GLE

	qglActiveTextureARB = (void *) eglGetProcAddress("glActiveTextureARB");
	qglClientActiveTextureARB = (void *) eglGetProcAddress("glClientActiveTextureARB");
	qglMultiTexCoord2fARB = (void *) eglGetProcAddress("glMultiTexCoord2fARB");
	qglLockArraysEXT = (void *) eglGetProcAddress("glLockArraysEXT");
	qglUnlockArraysEXT = (void *) eglGetProcAddress("glUnlockArraysEXT");
}

static qboolean CreateContext (void)
{
	static const EGLint config_attribs[] =
	{
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};
	static const EGLint context_attribs[] =
	{
		EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
		EGL_NONE
	};
	EGLConfig	config;
	EGLint		num_configs = 0;

	s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (s_display == EGL_NO_DISPLAY || !eglInitialize(s_display, NULL, NULL))
		return qfalse;
	if (!eglBindAPI(EGL_OPENGL_API))
		return qfalse;
	if (!eglChooseConfig(s_display, config_attribs, &config, 1, &num_configs) || num_configs == 0)
		return qfalse;

	s_surface = eglCreateWindowSurface(s_display, config, nwindowGetDefault(), NULL);
	if (s_surface == EGL_NO_SURFACE)
		return qfalse;
	s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, context_attribs);
	if (s_context == EGL_NO_CONTEXT)
		return qfalse;
	if (!eglMakeCurrent(s_display, s_surface, s_surface, s_context))
		return qfalse;

	eglSwapInterval(s_display, 1);
	return qtrue;
}

void GLimp_Init (qboolean fixedFunction)
{
	const char	*version;
	GLint		units = 0;

	ri.Sys_GLimpInit();

	if (!CreateContext())
		ri.Error(ERR_FATAL, "GLimp_Init: could not start OpenGL (EGL error 0x%x)", eglGetError());

	GetProcAddresses();

	version = (const char *) qglGetString(GL_VERSION);
	if (!version)
		ri.Error(ERR_FATAL, "GLimp_Init: GL_VERSION is NULL");
	sscanf(version, "%d.%d", &qglMajorVersion, &qglMinorVersion);

	glConfig.vidWidth = SCREEN_WIDTH;
	glConfig.vidHeight = SCREEN_HEIGHT;
	glConfig.windowAspect = (float) SCREEN_WIDTH / SCREEN_HEIGHT;
	glConfig.isFullscreen = qtrue;
	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;
	glConfig.displayFrequency = 60;
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = qfalse;

	Q_strncpyz(glConfig.vendor_string, (char *) qglGetString(GL_VENDOR), sizeof(glConfig.vendor_string));
	Q_strncpyz(glConfig.renderer_string, (char *) qglGetString(GL_RENDERER), sizeof(glConfig.renderer_string));
	Q_strncpyz(glConfig.version_string, version, sizeof(glConfig.version_string));
	Q_strncpyz(glConfig.extensions_string, (char *) qglGetString(GL_EXTENSIONS), sizeof(glConfig.extensions_string));

	glConfig.textureCompression = TC_NONE;
	glConfig.textureEnvAddAvailable = qtrue;
	qglGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &units);
	glConfig.numTextureUnits = units;

	NX_LogPrintf("OpenGL: %s, %s, %d texture units\n", glConfig.renderer_string, version, units);

	ri.Cvar_Get("r_availableModes", "", CVAR_ROM);
	ri.IN_Init(NULL);
}

void GLimp_Shutdown (void)
{
	ri.IN_Shutdown();

	if (s_display != EGL_NO_DISPLAY)
	{
		eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (s_context != EGL_NO_CONTEXT)
			eglDestroyContext(s_display, s_context);
		if (s_surface != EGL_NO_SURFACE)
			eglDestroySurface(s_display, s_surface);
		eglTerminate(s_display);
	}
	s_display = EGL_NO_DISPLAY;
	s_context = EGL_NO_CONTEXT;
	s_surface = EGL_NO_SURFACE;
}

void GLimp_EndFrame (void)
{
	eglSwapBuffers(s_display, s_surface);
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
	(void) red;
	(void) green;
	(void) blue;
}
