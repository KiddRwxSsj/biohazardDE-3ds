#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "s3e.h"

/*
 * Buttons on top of the game's touch UI. The touch manager lives at
 * *(image+0x171000): pointer array at +4, count at +8. CIwTouchButton and
 * CAnalogueTouchPad both keep an int16 rect at +0x60 and an active byte at +0x75.
 * Pad active = gameplay, the circle pad drags it and buttons hold the nearest
 * HUD icon. Otherwise the d-pad walks a highlight and A taps it.
 */

#define TOUCH_MANAGER 0x171000
#define VT_BUTTON 0x142f68
#define VT_ANALOGUE_PAD 0x152c28

#define MAX_TARGETS 64
#define CPAD_MAX 156
/* worn old 3DS circle pads rest well off centre, anything under this is drift */
#define CPAD_DEADZONE 44
#define TAP_MS 120
#define FIRST_BUTTON_ID 2

typedef struct {
	int x, y, w, h;
} rect;

typedef struct {
	const char *name;
	uint32_t key;
	int ref_x, ref_y;
	int id;
	int down;
	int at_x, at_y;
} hud_button;

static hud_button hud[] = {
	{ "HudX", KEY_X, 32, 32 },
	{ "HudB", KEY_B, 455, 300 },
	{ "HudSelect", KEY_SELECT, 32, 32 },
	{ "HudStart", KEY_START, 32, 32 },
};

#define NHUD (sizeof(hud) / sizeof(hud[0]))

static rect targets[MAX_TARGETS];
static int ntargets;
static int in_game;
static rect pad;

static int selected = -1;
static rect selected_rect;

static int finger_down, finger_x, finger_y;
static uint64_t tap_until;
static int tap_pending;
static uint32_t pad_frame;
static int pad_held;
static uint64_t pad_wait;

static uint32_t prev_held;
static int loaded;

static void load(void)
{
	for (unsigned i = 0; i < NHUD; i++) {
		const char *v = config_get("PORT", hud[i].name);
		if (v) {
			char *end;
			hud[i].ref_x = strtol(v, &end, 10);
			if (*end == ':')
				hud[i].ref_y = strtol(end + 1, NULL, 10);
		}
		hud[i].id = FIRST_BUTTON_ID + i;
	}
	loaded = 1;
}

static int16_t rd16(const uint8_t *p)
{
	return (int16_t)(p[0] | p[1] << 8);
}

static rect obj_rect(const uint8_t *o)
{
	rect r = { rd16(o + 0x60), rd16(o + 0x62), rd16(o + 0x64), rd16(o + 0x66) };
	return r;
}

static int on_screen(const rect *r)
{
	return r->w > 0 && r->h > 0 && r->x + r->w > 0 && r->y + r->h > 0 &&
		r->x < SURFACE_W && r->y < SURFACE_H;
}

/* heap pointers the game hands us all live in the app heap, anything else means we read junk */
static int sane(const void *p)
{
	uint32_t a = (uint32_t)p;
	return a >= 0x08000000 && a < 0x10000000 && !(a & 3);
}

static void scan(void)
{
	uint8_t *img = game_image();
	uint8_t *mgr;
	uint8_t **arr;
	uint32_t n;

	ntargets = 0;
	in_game = 0;
	if (!img)
		return;
	mgr = *(uint8_t **)(img + TOUCH_MANAGER);
	if (!sane(mgr))
		return;
	arr = *(uint8_t ***)(mgr + 4);
	n = *(uint32_t *)(mgr + 8);
	if (!sane(arr) || n > 1024)
		return;

	for (uint32_t i = 0; i < n; i++) {
		uint8_t *o = arr[i];
		if (!sane(o) || !o[0x75])
			continue;
		uint32_t vt = *(uint32_t *)o - (uint32_t)img;
		rect r = obj_rect(o);
		if (!on_screen(&r))
			continue;
		if (vt == VT_ANALOGUE_PAD) {
			in_game = 1;
			pad = r;
		} else if (vt == VT_BUTTON && r.h > 20 && ntargets < MAX_TARGETS) {
			targets[ntargets++] = r;
		}
	}
}

static int cx(const rect *r)
{
	return r->x + r->w / 2;
}

static int cy(const rect *r)
{
	return r->y + r->h / 2;
}

static int nearest(int x, int y, int max)
{
	int best = -1, best_d = max * max;
	for (int i = 0; i < ntargets; i++) {
		int dx = cx(&targets[i]) - x, dy = cy(&targets[i]) - y;
		int d = dx * dx + dy * dy;
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	return best;
}

static void tap(const rect *r)
{
	finger_x = cx(r);
	finger_y = cy(r);
	tap_pending = 1;
	tap_until = 0;
}

/* pick the closest target whose centre lies in the pressed direction, favouring straight lines */
static int step(int from, int dx, int dy)
{
	int best = -1, best_score = 0x7fffffff;
	int fx = cx(&targets[from]), fy = cy(&targets[from]);

	for (int i = 0; i < ntargets; i++) {
		if (i == from)
			continue;
		int ox = cx(&targets[i]) - fx, oy = cy(&targets[i]) - fy;
		int along = ox * dx + oy * dy;
		if (along <= 0)
			continue;
		int across = abs(ox * dy - oy * dx);
		int score = along + across * 3;
		if (score < best_score) {
			best_score = score;
			best = i;
		}
	}
	return best;
}

/* corner icons (back, pages, skip) are reachable with their own buttons, so lists come first */
static int is_corner(const rect *r)
{
	int x = cx(r), y = cy(r);
	return (x < 80 || x > SURFACE_W - 80) && (y < 80 || y > SURFACE_H - 80);
}

static int first_target(void)
{
	int best = -1;
	for (int pass = 0; pass < 2 && best < 0; pass++) {
		for (int i = 0; i < ntargets; i++) {
			if (!pass && is_corner(&targets[i]))
				continue;
			if (best < 0 || targets[i].y < targets[best].y ||
			    (targets[i].y == targets[best].y && targets[i].x < targets[best].x))
				best = i;
		}
	}
	return best;
}

static uint32_t layout_hash(void)
{
	uint32_t h = 0;
	for (int i = 0; i < ntargets; i++)
		if (!is_corner(&targets[i]))
			h = h * 31 + (targets[i].x * 7 + targets[i].y * 13 + targets[i].w * 17 + targets[i].h);
	return h;
}

static void menu_poll(uint32_t held, uint32_t pressed)
{
	static uint32_t last_layout;
	uint32_t layout = layout_hash();
	int keep = -1;

	/* a new screen (or one that just finished sliding in) starts on its first item */
	if (layout != last_layout) {
		last_layout = layout;
		selected_rect.w = 0;
	}
	for (int i = 0; i < ntargets; i++)
		if (!memcmp(&targets[i], &selected_rect, sizeof(rect)))
			keep = i;
	selected = keep >= 0 ? keep : first_target();
	if (selected < 0)
		return;

	if (pressed & KEY_DUP) {
		int n = step(selected, 0, -1);
		if (n >= 0) selected = n;
	}
	if (pressed & KEY_DDOWN) {
		int n = step(selected, 0, 1);
		if (n >= 0) selected = n;
	}
	if (pressed & KEY_DLEFT) {
		int n = step(selected, -1, 0);
		if (n >= 0) selected = n;
	}
	if (pressed & KEY_DRIGHT) {
		int n = step(selected, 1, 0);
		if (n >= 0) selected = n;
	}
	selected_rect = targets[selected];

	if (pressed & KEY_A)
		tap(&targets[selected]);

	/* back sits bottom left on every menu, pages and skip on the top corners */
	if (pressed & KEY_B) {
		int n = nearest(16, 300, 90);
		if (n >= 0) tap(&targets[n]);
	}
	if (pressed & KEY_L) {
		int n = nearest(25, 20, 90);
		if (n >= 0) tap(&targets[n]);
	}
	if (pressed & (KEY_R | KEY_X)) {
		int n = nearest(455, 20, 90);
		if (n >= 0) tap(&targets[n]);
	}
}

/*
 * The phone has one gun icon: first tap raises the gun, later taps fire. The
 * knife icon works the same way, and while either is out a running man icon top
 * right puts it away. Near a door or anything usable the gun icon shows that
 * action instead. R holds the gun up, L the knife, A uses whichever is out or
 * does the action.
 *
 * The icon (CGuiTouchIcon) shows the image at +0x50; +0x90 is its own image list,
 * [0] aim and [1] fire. Anything else in +0x50 is a context action.
 */
#define AIM_X 419
#define AIM_Y 150
#define KNIFE_X 346
#define KNIFE_Y 244
#define CANCEL_X 419
#define CANCEL_Y 44
#define GUN_ID (FIRST_BUTTON_ID + NHUD)
#define RETRY_MS 300

enum { ICON_NONE, ICON_AIM, ICON_FIRE, ICON_ACTION };
enum { HELD_NONE, HELD_GUN, HELD_KNIFE };

static void *aim_icon;

static struct {
	int down, x, y;
	uint64_t until, retry, raised_at;
	int want, mine, held, was_up, cancel;
} gun;

static int icon_state(void)
{
	uint8_t *o = aim_icon;

	if (!sane(o))
		return ICON_NONE;
	uint32_t cur = *(uint32_t *)(o + 0x50);
	if (cur == *(uint32_t *)(o + 0x90))
		return ICON_AIM;
	if (cur == *(uint32_t *)(o + 0x94))
		return ICON_FIRE;
	return ICON_ACTION;
}

static void gun_touch(int down, int x, int y)
{
	if (gun.down && (!down || x != gun.x || y != gun.y)) {
		touch_send(GUN_ID, 0, gun.x, gun.y);
		gun.down = 0;
	}
	if (down && !gun.down) {
		touch_send(GUN_ID, 1, x, y);
		gun.down = 1;
		gun.x = x;
		gun.y = y;
	}
}

static void gun_tap(const rect *r, uint64_t now)
{
	gun_touch(1, cx(r), cy(r));
	gun.until = now + TAP_MS;
	gun.retry = now + RETRY_MS;
}

/* returns 1 when the weapon just went away, the pad has to be grabbed again then */
static int gun_poll(uint32_t held, uint32_t pressed, uint32_t released)
{
	uint64_t now = s3eTimerGetMs();
	int aim = nearest(AIM_X, AIM_Y, 80);
	int knife = nearest(KNIFE_X, KNIFE_Y, 60);
	int cancel = nearest(CANCEL_X, CANCEL_Y, 30);
	int up = cancel >= 0;
	int ended = gun.was_up && !up;

	gun.was_up = up;
	if (pressed & KEY_R)
		gun.want = HELD_GUN;
	if (pressed & KEY_L)
		gun.want = HELD_KNIFE;
	if ((released & KEY_R) && gun.want == HELD_GUN)
		gun.want = HELD_NONE;
	if ((released & KEY_L) && gun.want == HELD_KNIFE)
		gun.want = HELD_NONE;
	if (pressed & KEY_Y) {
		gun.want = HELD_NONE;
		gun.cancel = 1;
	}
	if (!up)
		gun.cancel = 0;
	/* a raise that never took (the knife takes a while to come out), not ours to put away */
	if (!up && gun.mine && !gun.want && now > gun.raised_at + 2000)
		gun.mine = 0;
	if (!up && !gun.mine)
		gun.held = HELD_NONE;
	if (now < gun.until)
		return ended;

	int a = gun.held == HELD_KNIFE ? knife : aim;
	if (gun.want && !up && now >= gun.retry &&
			(gun.want == HELD_KNIFE ? knife >= 0 : aim >= 0 && icon_state() == ICON_AIM)) {
		gun_tap(&targets[gun.want == HELD_KNIFE ? knife : aim], now);
		/* another tap while it's still coming out would be an attack */
		gun.retry = now + 1500;
		gun.mine = 1;
		gun.held = gun.want;
		gun.raised_at = now;
	} else if (!gun.want && up && (gun.mine || gun.cancel) && now >= gun.retry) {
		/* the game ignores the tap while an attack is still going, so keep at it */
		gun_tap(&targets[cancel], now);
	} else if ((held & KEY_A) && up && a >= 0) {
		gun_touch(1, cx(&targets[a]), cy(&targets[a]));
	} else if ((held & KEY_A) && !up && aim >= 0 && icon_state() == ICON_ACTION) {
		gun_touch(1, cx(&targets[aim]), cy(&targets[aim]));
	} else {
		gun_touch(0, 0, 0);
	}
	return ended;
}

static void hud_release_all(void)
{
	for (unsigned i = 0; i < NHUD; i++) {
		if (hud[i].down) {
			touch_send(hud[i].id, 0, hud[i].at_x, hud[i].at_y);
			hud[i].down = 0;
		}
	}
	/* the pad blinks off for a frame when the knife comes out, so keep the weapon state */
	gun_touch(0, 0, 0);
	gun.until = 0;
}

static void game_poll(uint32_t held, uint32_t pressed, uint32_t released)
{
	circlePosition cp;
	int dx, dy;

	/*
	 * While aiming the stick aims. A finger still down when that ends never
	 * walks, and one pressed again too soon is ignored while Leon puts it away.
	 */
	if (gun_poll(held, pressed, released))
		pad_wait = s3eTimerGetMs() + 200;
	if (s3eTimerGetMs() < pad_wait) {
		pad_held = 0;
		finger_down = 0;
		return;
	}

	for (unsigned i = 0; i < NHUD; i++) {
		int want = !!(held & hud[i].key);
		if (want && !hud[i].down) {
			int n = nearest(hud[i].ref_x, hud[i].ref_y, 80);
			if (n < 0)
				continue;
			hud[i].down = 1;
			hud[i].at_x = cx(&targets[n]);
			hud[i].at_y = cy(&targets[n]);
			touch_send(hud[i].id, 1, hud[i].at_x, hud[i].at_y);
		} else if (!want && hud[i].down) {
			hud[i].down = 0;
			touch_send(hud[i].id, 0, hud[i].at_x, hud[i].at_y);
		}
	}

	hidCircleRead(&cp);
	dx = cp.dx;
	dy = cp.dy;
	if (held & KEY_DLEFT) dx = -CPAD_MAX;
	if (held & KEY_DRIGHT) dx = CPAD_MAX;
	if (held & KEY_DUP) dy = CPAD_MAX;
	if (held & KEY_DDOWN) dy = -CPAD_MAX;
	if (dx > CPAD_MAX) dx = CPAD_MAX;
	if (dx < -CPAD_MAX) dx = -CPAD_MAX;
	if (dy > CPAD_MAX) dy = CPAD_MAX;
	if (dy < -CPAD_MAX) dy = -CPAD_MAX;

	if (dx * dx + dy * dy <= CPAD_DEADZONE * CPAD_DEADZONE) {
		pad_held = 0;
		finger_down = 0;
		return;
	}

	/* the pad only latches a finger that lands on its centre and stays there for a frame */
	int r = pad.w * 35 / 100;
	int gx = cx(&pad) + dx * r / CPAD_MAX;
	int gy = cy(&pad) - dy * r / CPAD_MAX;
	uint32_t frame = port_frame();
	if (!pad_held) {
		pad_held = 1;
		pad_frame = frame;
		finger_x = cx(&pad);
		finger_y = cy(&pad);
	} else if (frame != pad_frame) {
		pad_frame = frame;
		finger_x += (gx - finger_x) / 2 ? (gx - finger_x) / 2 : gx - finger_x;
		finger_y += (gy - finger_y) / 2 ? (gy - finger_y) / 2 : gy - finger_y;
	}
	finger_down = 1;
}

void controls_poll(void)
{
	uint32_t held = hidKeysHeld() | auto_keys();
	uint32_t pressed = held & ~prev_held;
	uint32_t released = prev_held & ~held;
	circlePosition cp;

	prev_held = held;
	if (!loaded)
		load();

	scan();

	/* a tap holds the finger for a few frames, the game polls touches once per frame */
	if (tap_pending) {
		uint64_t now = s3eTimerGetMs();
		if (!tap_until)
			tap_until = now + TAP_MS;
		finger_down = now < tap_until;
		if (!finger_down)
			tap_pending = 0;
		return;
	}

	if (in_game) {
		selected = -1;
		game_poll(held, pressed, released);
		return;
	}

	hud_release_all();
	pad_held = 0;
	finger_down = 0;

	/* the circle pad also moves the highlight, one step per push */
	hidCircleRead(&cp);
	static uint32_t cpad_prev;
	uint32_t cpad = 0;
	if (cp.dy > 100) cpad |= KEY_DUP;
	if (cp.dy < -100) cpad |= KEY_DDOWN;
	if (cp.dx < -100) cpad |= KEY_DLEFT;
	if (cp.dx > 100) cpad |= KEY_DRIGHT;
	pressed |= cpad & ~cpad_prev;
	cpad_prev = cpad;

	menu_poll(held, pressed);
}

int controls_finger(int *x, int *y)
{
	if (!finger_down)
		return 0;
	*x = finger_x;
	*y = finger_y;
	return 1;
}

int controls_highlight(int *x, int *y, int *w, int *h)
{
	if (in_game || selected < 0 || selected >= ntargets)
		return 0;
	*x = targets[selected].x;
	*y = targets[selected].y;
	*w = targets[selected].w;
	*h = targets[selected].h;
	return 1;
}

/*
 * CGuiTouchIcon renders at image+0x5b29c, the bl at 0x5b302 (to the element
 * render at 0xf9b8) is the only actual draw, so it's pointed at hud_icon_draw.
 * CGuiAnaloguePad render is vtable slot 14; 0x4f4b8 is its touch update alone.
 * Both renders refresh the touch rects the controls rely on, so they keep running.
 */
#define VT_GUI_ANALOGUE_PAD 0x14c7e0
#define ICON_DRAW_CALL 0x5b302
#define ELEMENT_RENDER 0xf9b8
#define PAD_TOUCH_UPDATE 0x4f4b8
#define RENDER_SLOT (14 * 4)

typedef void (*render_fn)(void *self, void *ctx);
static render_fn element_render, pad_touch_update, pad_render;

/* the icon's touch button sits at +0x58, its rect was refreshed just before this call */
static void hud_icon_draw(void *self, void *ctx)
{
	uint8_t *button = *(uint8_t **)((uint8_t *)self + 0x58);

	if (sane(button)) {
		rect r = obj_rect(button);
		if (abs(cx(&r) - AIM_X) < 30 && abs(cy(&r) - AIM_Y) < 30)
			aim_icon = self;
		if (gl_bottom_ui())
			gl_icon_rect(r.x, r.y, r.w, r.h, 0);
	}
	if (gl_bottom_ui() || !in_game)
		element_render(self, ctx);
}

static void hud_pad_render(void *self, void *ctx)
{
	if (gl_bottom_ui()) {
		if (in_game)
			gl_icon_rect(pad.x, pad.y, pad.w, pad.h, 1);
		pad_render(self, ctx);
	} else if (in_game) {
		pad_touch_update(self, ctx);
	} else {
		pad_render(self, ctx);
	}
}

void hud_hooks(uint8_t *image)
{
	uint32_t *slot = (uint32_t *)(image + VT_GUI_ANALOGUE_PAD + RENDER_SLOT);

	element_render = (render_fn)(image + ELEMENT_RENDER + 1);
	pad_touch_update = (render_fn)(image + PAD_TOUCH_UPDATE + 1);
	pad_render = (render_fn)*slot;

	if (hook_call(image, ICON_DRAW_CALL, hud_icon_draw))
		return;
	*slot = (uint32_t)hud_pad_render;
}
