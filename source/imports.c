#include <string.h>

#include "s3e.h"
#include "gl.h"

void *eglGetDisplay(void *native);
uint32_t eglInitialize(void *dpy, int32_t *major, int32_t *minor);
uint32_t eglChooseConfig(void *dpy, const int32_t *attr, void **configs, int32_t size, int32_t *num);
uint32_t eglGetConfigs(void *dpy, void **configs, int32_t size, int32_t *num);
uint32_t eglGetConfigAttrib(void *dpy, void *config, int32_t attr, int32_t *value);
void *eglCreateWindowSurface(void *dpy, void *config, void *win, const int32_t *attr);
void *eglCreateContext(void *dpy, void *config, void *share, const int32_t *attr);
uint32_t eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
uint32_t eglQuerySurface(void *dpy, void *surf, int32_t attr, int32_t *value);
int32_t eglGetError(void);
const char *eglQueryString(void *dpy, int32_t name);
void *eglGetCurrentDisplay(void);
void *eglGetCurrentContext(void);
void *eglGetCurrentSurface(int32_t which);
uint32_t eglSwapBuffers(void *dpy, void *surf);
uint32_t egl_true(void);

typedef struct {
	const char *name;
	void *fn;
} import;

#define F(x) { #x, (void *)x }
#define AS(x, y) { #x, (void *)y }

static const import imports[] = {
	F(s3eMallocBase),
	F(s3eReallocBase),
	F(s3eFreeBase),
	F(s3eMemoryGetInt),
	F(s3eMemorySetInt),
	F(s3eMemoryHeapCreate),
	F(s3eMemorySetUserMemMgr),

	F(s3eConfigGetInt),
	F(s3eConfigGetString),

	F(s3eFileOpen),
	F(s3eFileOpenFromMemory),
	F(s3eFileClose),
	F(s3eFileRead),
	F(s3eFileWrite),
	F(s3eFileSeek),
	F(s3eFileTell),
	F(s3eFileGetSize),
	F(s3eFileGetChar),
	F(s3eFileFlush),
	F(s3eFileCheckExists),
	F(s3eFileDelete),
	F(s3eFileMakeDirectory),
	F(s3eFileGetError),
	F(s3eFileAddUserFileSys),
	F(s3eSecureStorageGet),
	F(s3eSecureStoragePut),

	F(s3eCompressionDecompInit),
	F(s3eCompressionDecompRead),
	F(s3eCompressionDecompFinal),

	F(s3eDeviceGetInt),
	F(s3eDeviceGetString),
	F(s3eDeviceCheckQuitRequest),
	F(s3eDeviceCheckPauseRequest),
	F(s3eDeviceYield),
	F(s3eDeviceYieldUntilEvent),
	F(s3eDeviceExit),

	F(s3eTimerGetMs),
	F(s3eTimerGetUTC),
	F(s3eTimerGetInt),
	F(s3eTimerGetLocaltimeOffset),

	F(s3eDebugTraceLine),
	F(s3eDebugOutputString),
	F(s3eDebugPrint),
	F(s3eDebugErrorShow),
	F(s3eDebugAssertShow),

	F(s3eSurfaceGetInt),
	F(s3eGLGetInt),
	F(s3eGLGetNativeWindow),

	F(s3ePointerRegister),
	F(s3ePointerUnRegister),
	F(s3ePointerUpdate),
	F(s3ePointerGetState),
	F(s3ePointerGetX),
	F(s3ePointerGetY),
	F(s3ePointerGetInt),

	F(s3eKeyboardRegister),
	F(s3eKeyboardUnRegister),
	F(s3eKeyboardUpdate),
	F(s3eKeyboardGetState),
	F(s3eKeyboardAnyKey),
	F(s3eKeyboardClearState),
	F(s3eKeyboardSetInt),
	F(s3eKeyboardGetDisplayName),

	F(s3eSoundGetInt),
	F(s3eSoundSetInt),
	F(s3eSoundGetFreeChannel),
	F(s3eSoundChannelRegister),
	F(s3eSoundChannelUnRegister),
	F(s3eSoundChannelPlay),
	F(s3eSoundChannelStop),
	F(s3eSoundChannelPause),
	F(s3eSoundChannelResume),
	F(s3eSoundChannelGetInt),
	F(s3eSoundChannelSetInt),
	F(s3eExtGet),

	F(eglGetDisplay),
	F(eglInitialize),
	F(eglChooseConfig),
	F(eglGetConfigs),
	F(eglGetConfigAttrib),
	F(eglCreateWindowSurface),
	F(eglCreateContext),
	F(eglMakeCurrent),
	F(eglQuerySurface),
	F(eglGetError),
	F(eglQueryString),
	F(eglGetCurrentDisplay),
	F(eglGetCurrentContext),
	F(eglGetCurrentSurface),
	F(eglSwapBuffers),
	AS(eglSwapInterval, egl_true),
	AS(eglBindAPI, egl_true),
	AS(eglWaitGL, egl_true),
	AS(eglWaitNative, egl_true),
	AS(eglWaitClient, egl_true),
	AS(eglDestroySurface, egl_true),
	AS(eglDestroyContext, egl_true),
	AS(eglTerminate, egl_true),
	AS(eglSurfaceAttrib, egl_true),
	AS(eglReleaseThread, egl_true),

	F(glMatrixMode),
	F(glLoadIdentity),
	F(glLoadMatrixx),
	F(glLoadMatrixf),
	F(glPushMatrix),
	F(glPopMatrix),
	F(glTranslatex),
	F(glScalex),
	F(glRotatex),
	F(glOrthox),
	F(glFrustumx),

	F(glGenBuffers),
	F(glDeleteBuffers),
	F(glBindBuffer),
	F(glBufferData),

	F(glVertexPointer),
	F(glColorPointer),
	F(glNormalPointer),
	F(glTexCoordPointer),
	F(glEnableClientState),
	F(glDisableClientState),
	F(glActiveTexture),
	F(glClientActiveTexture),

	F(glEnable),
	F(glDisable),
	F(glColor4x),
	F(glBlendFunc),
	F(glAlphaFuncx),
	F(glDepthFunc),
	F(glDepthMask),
	F(glColorMask),
	F(glCullFace),
	F(glFrontFace),
	F(glViewport),
	F(glScissor),
	F(glDepthRangex),
	F(glDepthRangef),
	F(glClearColorx),
	F(glClearColor),
	F(glClearDepthf),
	F(glPixelStorei),
	F(glFogx),
	F(glFogxv),

	F(glGenTextures),
	F(glDeleteTextures),
	F(glBindTexture),
	F(glTexParameteri),
	F(glTexParameterx),
	F(glTexImage2D),
	F(glTexSubImage2D),
	F(glTexEnvx),

	F(glDrawArrays),
	F(glDrawElements),
	F(glClear),
	F(glReadPixels),
	F(glGetString),
	F(glGetIntegerv),
	F(glGetBooleanv),
};

void *import_lookup(const char *name)
{
	for (size_t i = 0; i < sizeof(imports) / sizeof(imports[0]); i++)
		if (!strcmp(imports[i].name, name))
			return imports[i].fn;
	return NULL;
}
