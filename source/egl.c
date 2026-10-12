#include <3ds.h>
#include <string.h>

#include "port.h"
#include "gl.h"

#define EGL_SUCCESS 0x3000
#define EGL_BUFFER_SIZE 0x3020
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_CONFIG_ID 0x3028
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057

#define FAKE_DISPLAY ((void *)1)
#define FAKE_CONFIG ((void *)1)
#define FAKE_SURFACE ((void *)0x51)
#define FAKE_CONTEXT ((void *)0xc1)

static uint32_t frames;

void *eglGetDisplay(void *native)
{
	return FAKE_DISPLAY;
}

uint32_t eglInitialize(void *dpy, int32_t *major, int32_t *minor)
{
	if (major)
		*major = 1;
	if (minor)
		*minor = 1;
	return 1;
}

uint32_t eglChooseConfig(void *dpy, const int32_t *attr, void **configs, int32_t size, int32_t *num)
{
	if (configs && size)
		configs[0] = FAKE_CONFIG;
	if (num)
		*num = 1;
	return 1;
}

uint32_t eglGetConfigs(void *dpy, void **configs, int32_t size, int32_t *num)
{
	return eglChooseConfig(dpy, NULL, configs, size, num);
}

uint32_t eglGetConfigAttrib(void *dpy, void *config, int32_t attr, int32_t *value)
{
	switch (attr) {
	case EGL_BUFFER_SIZE: *value = 16; break;
	case EGL_RED_SIZE: *value = 5; break;
	case EGL_GREEN_SIZE: *value = 6; break;
	case EGL_BLUE_SIZE: *value = 5; break;
	case EGL_DEPTH_SIZE: *value = 16; break;
	case EGL_CONFIG_ID: *value = 1; break;
	case EGL_SURFACE_TYPE: *value = 4; break;
	case EGL_RENDERABLE_TYPE: *value = 1; break;
	default: *value = 0; break;
	}
	return 1;
}

void *eglCreateWindowSurface(void *dpy, void *config, void *win, const int32_t *attr)
{
	return FAKE_SURFACE;
}

void *eglCreateContext(void *dpy, void *config, void *share, const int32_t *attr)
{
	return FAKE_CONTEXT;
}

uint32_t eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx)
{
	return 1;
}

uint32_t eglQuerySurface(void *dpy, void *surf, int32_t attr, int32_t *value)
{
	if (attr == EGL_WIDTH)
		*value = SURFACE_W;
	else if (attr == EGL_HEIGHT)
		*value = SURFACE_H;
	else
		*value = 0;
	return 1;
}

int32_t eglGetError(void)
{
	return EGL_SUCCESS;
}

const char *eglQueryString(void *dpy, int32_t name)
{
	return "1.1";
}

void *eglGetCurrentDisplay(void)
{
	return FAKE_DISPLAY;
}

void *eglGetCurrentContext(void)
{
	return FAKE_CONTEXT;
}

void *eglGetCurrentSurface(int32_t which)
{
	return FAKE_SURFACE;
}

uint32_t port_frame(void)
{
	return frames;
}

uint32_t egl_true(void)
{
	return 1;
}

uint32_t eglSwapBuffers(void *dpy, void *surf)
{
	if (!frames)
		port_log("first frame\n");
	frames++;
	if (!(frames % 300))
		gl_stats();
	gl_swap();
	gl_perf();
	port_pump();
	return 1;
}
