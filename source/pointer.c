#include <3ds.h>
#include <stdlib.h>

#include "s3e.h"

enum {
	EV_BUTTON,
	EV_MOTION,
	EV_TOUCH,
	EV_TOUCH_MOTION,
	EV_COUNT
};

enum {
	STATE_UP = 0,
	STATE_DOWN = 1,
	STATE_PRESSED = 2,
	STATE_RELEASED = 4,
};

typedef struct {
	uint32_t id;
	uint32_t pressed;
	int32_t x;
	int32_t y;
} touch_event;

typedef struct {
	uint32_t id;
	int32_t x;
	int32_t y;
} touch_motion_event;

static struct {
	s3e_callback cb;
	void *user;
} handlers[EV_COUNT];

static int touching, down, was_down, x, y;

static int trace = -1;

static void fire(int ev, void *data)
{
	if (trace < 0)
		trace = port_option("Trace", 0);
	if (trace && ev >= EV_TOUCH) {
		int32_t *d = data;
		if (ev == EV_TOUCH)
			port_log("touch %ld %s %ld,%ld%s\n", d[0], d[1] ? "down" : "up", d[2], d[3], handlers[ev].cb ? "" : " (no handler)");
		else
			port_log("touch %ld move %ld,%ld\n", d[0], d[1], d[2]);
	}
	if (handlers[ev].cb)
		handlers[ev].cb(data, handlers[ev].user);
}

void touch_send(uint32_t id, int pressed, int tx, int ty)
{
	touch_event e = { id, pressed, tx, ty };
	fire(EV_TOUCH, &e);
}

/* [PORT] AutoTaps=ms:x:y,... in surface coords, held for 150 ms. Only for unattended test runs. */
static int auto_tap(int *tx, int *ty)
{
	static const char *list;
	static int loaded;
	static uint64_t start;
	uint64_t now = s3eTimerGetMs();

	if (!start)
		start = now;
	now -= start;

	if (!loaded) {
		list = config_get("PORT", "AutoTaps");
		loaded = 1;
	}
	for (const char *p = list; p && *p;) {
		char *end;
		uint32_t f = strtoul(p, &end, 10);
		int ax = strtol(end + 1, &end, 10);
		int ay = strtol(end + 1, &end, 10);
		if (now >= f && now < f + 150) {
			*tx = ax;
			*ty = ay;
			return 1;
		}
		p = *end ? end + 1 : end;
	}
	return 0;
}

/* bottom screen is 320x240, the game thinks it's on a 480x320 phone */
void pointer_poll(void)
{
	touchPosition t;
	int nx = x, ny = y;
	int now = !!(hidKeysHeld() & KEY_TOUCH);

	controls_poll();

	if (now) {
		hidTouchRead(&t);
		nx = t.px * SURFACE_W / 320;
		ny = t.py * SURFACE_H / 240;
	} else {
		now = auto_tap(&nx, &ny) || controls_finger(&nx, &ny);
	}

	if (now != touching) {
		touching = now;
		x = nx;
		y = ny;
		touch_event e = { 0, now, x, y };
		fire(EV_TOUCH, &e);
		fire(EV_BUTTON, &e);
	} else if (now && (nx != x || ny != y)) {
		x = nx;
		y = ny;
		touch_motion_event m = { 0, x, y };
		int32_t xy[2] = { x, y };
		fire(EV_TOUCH_MOTION, &m);
		fire(EV_MOTION, xy);
	}
}

int32_t s3ePointerRegister(int32_t ev, s3e_callback cb, void *user)
{
	if (ev < 0 || ev >= EV_COUNT)
		return S3E_RESULT_ERROR;
	if (port_option("Trace", 0))
		port_log("pointer event %ld registered\n", ev);
	handlers[ev].cb = cb;
	handlers[ev].user = user;
	return S3E_RESULT_SUCCESS;
}

int32_t s3ePointerUnRegister(int32_t ev, s3e_callback cb)
{
	if (ev < 0 || ev >= EV_COUNT || handlers[ev].cb != cb)
		return S3E_RESULT_ERROR;
	handlers[ev].cb = NULL;
	return S3E_RESULT_SUCCESS;
}

int32_t s3ePointerUpdate(void)
{
	was_down = down;
	down = touching;
	return S3E_RESULT_SUCCESS;
}

int32_t s3ePointerGetState(int32_t button)
{
	if (button)
		return STATE_UP;
	if (down)
		return was_down ? STATE_DOWN : STATE_DOWN | STATE_PRESSED;
	return was_down ? STATE_RELEASED : STATE_UP;
}

int32_t s3ePointerGetX(void)
{
	return x;
}

int32_t s3ePointerGetY(void)
{
	return y;
}

int32_t s3ePointerGetInt(int32_t prop)
{
	return 1;
}
