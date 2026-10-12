#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "s3e.h"

#define MS_1900_TO_1970 2208988800000ULL

static int quit;

int port_quitting(void)
{
	return quit;
}

void port_pump(void)
{
	if (!aptMainLoop())
		quit = 1;
	hidScanInput();
	if (((hidKeysHeld() | auto_keys()) & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT))
		quit = 1;
	pointer_poll();
}

/* still guesses lifted from the emulator, check against libjirobiodg.so */
int32_t s3eDeviceGetInt(int32_t prop)
{
	switch (prop) {
	case 0: return 12;
	case 9: return 1;
	}
	return 0;
}

const char *s3eDeviceGetString(int32_t prop)
{
	return "3DS";
}

int32_t s3eDeviceCheckQuitRequest(void)
{
	return quit;
}

int32_t s3eDeviceCheckPauseRequest(void)
{
	return 0;
}

int32_t s3eDeviceYield(int32_t ms)
{
	port_pump();
	if (ms > 0)
		svcSleepThread((s64)ms * 1000000);
	return S3E_RESULT_SUCCESS;
}

int32_t s3eDeviceYieldUntilEvent(int32_t ms)
{
	return s3eDeviceYield(ms);
}

void s3eDeviceExit(int32_t code)
{
	port_exit(code);
}

uint64_t s3eTimerGetMs(void)
{
	return svcGetSystemTick() / CPU_TICKS_PER_MSEC;
}

uint64_t s3eTimerGetUTC(void)
{
	return osGetTime() - MS_1900_TO_1970;
}

int32_t s3eTimerGetInt(int32_t prop)
{
	return 1;
}

int32_t s3eTimerGetLocaltimeOffset(void)
{
	return 0;
}

int32_t s3eDebugTraceLine(const char *s)
{
	port_log("%s\n", s);
	return 0;
}

int32_t s3eDebugOutputString(const char *s)
{
	port_log("%s", s);
	return 0;
}

int32_t s3eDebugPrint(int32_t x, int32_t y, const char *s, int32_t wrap)
{
	return 0;
}

int32_t s3eDebugErrorShow(int32_t type, const char *s)
{
	port_log("ERROR: %s\n", s);
	return 0;
}

int32_t s3eDebugAssertShow(int32_t type, const char *s)
{
	port_log("ASSERT: %s\n", s);
	return 0;
}

int32_t s3eSurfaceGetInt(int32_t prop)
{
	switch (prop) {
	case 0:
	case 4: return SURFACE_W;
	case 1:
	case 5: return SURFACE_H;
	case 2:
	case 7: return SURFACE_W * 2;
	case 3:
	case 6: return 0x125;
	}
	return 0;
}

/* anything <= 0xff makes the engine give up on GL and fall back to its software renderer */
int32_t s3eGLGetInt(int32_t prop)
{
	return prop == 0 ? 0x110 : 1;
}

void *s3eGLGetNativeWindow(void)
{
	static int window;
	return &window;
}

int32_t s3eExtGet(const char *name, void *buf, int32_t size)
{
	return S3E_RESULT_ERROR;
}
