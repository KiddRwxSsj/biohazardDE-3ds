#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "port.h"
#include "gl.h"
#include "s3e.h"
#include "gles_shbin.h"
#include "simple_shbin.h"

#define RING_SIZE (768 * 1024)
#define STACK_DEPTH 32

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define FX(v) ((float)(int32_t)(v) * (1.0f / 65536.0f))

typedef struct {
	float m[16];
} mat4;

typedef struct {
	mat4 s[STACK_DEPTH];
	int top;
} mstack;

typedef struct {
	int size;
	uint32_t type;
	int stride;
	const void *ptr;
	uint32_t buffer;
	int enabled;
} array;

typedef struct {
	void *data;
	uint32_t size;
} buffer;

typedef struct {
	C3D_Tex tex;
	int alive;
	int vram;
	int hidden;
	uint32_t hash;
	uint32_t last_use;
	int w, h;
	uint32_t min_filter, mag_filter, wrap_s, wrap_t;
} texture;

typedef struct {
	uint32_t mode;
	uint32_t combine_rgb, combine_alpha;
	uint32_t src_rgb[3], src_alpha[3];
	uint32_t op_rgb[3], op_alpha[3];
	int scale_rgb, scale_alpha;
	uint32_t color;
} texenv;

typedef struct {
	float pos[3];
	float tc0[2];
	float tc1[2];
	uint8_t color[4];
} vertex;

static DVLB_s *dvlb;
static shaderProgram_s program;
static int u_mvp, u_tex0, u_tex1;
static C3D_RenderTarget *eyes[3];
static C3D_Tex white;

static uint8_t *ring[2];
static uint32_t ring_pos;
static int ring_idx, ring_full;
static uint32_t ring_peak, tex_bytes;
static int in_frame, cur_eye = -1;
static int stereo, stereo_seen;
static int trace_gpu, perf_log;
static int suppress;
static int skip_draws, skip_clears;
static uint32_t hide_hash[16];
static int nhide;
static uint32_t frame_no, tex_copies;
static uint32_t last_vblank, ndraws, nswitches;
static float cmdbuf_peak;
static vertex *vbase;
static int shadow_valid;
static int view_valid, view_cache[9];
static u64 build_ticks, state_ticks, submit_ticks;

/*
 * C3D_TexBind always resends the unit and clears the texture cache, even for the
 * same texture. We bind our own copies of the headers and skip the bind while the
 * copy matches and tex_gen (bumped on any upload/free/param change) hasn't moved.
 */
static uint32_t tex_gen = 1, unit_gen[2];
static C3D_Tex unit_copy[2];
static int fog_lut_bound;

/* controls.c records the touch icons' rects as the game renders them, draws inside those go below */
#define BOTTOM 2
#define MAX_ICONS 48
#define BG_HASH 0x6023ccef
static int bottom_ui;
static uint32_t bg_tex;
static struct { int x, y, w, h, whole; } icons[MAX_ICONS];
static int nicons;

static mstack modelview, projection, texmat[2];
static uint32_t matrix_mode = GL_MODELVIEW;

static array va_pos, va_color, va_normal, va_tc[2];
static int active_unit, client_unit;
static uint32_t array_buffer, element_buffer;
static float cur_color[4] = { 1, 1, 1, 1 };

static buffer *buffers;
static uint32_t nbuffers, next_buffer = 1;

static texture *textures;
static uint32_t ntextures, next_texture = 1;
static uint32_t bound[2];
static int tex_enabled[2];
static texenv env[2];
static int unpack_align = 4;
static int tex_rgb565 = 1, use_mipmaps = 1;
static u64 gl_ticks, wait_ticks, end_ticks, tex_ticks;
volatile const char *gl_stage = "-";

static void tex_image(int32_t level, int32_t w, int32_t h, uint32_t format, uint32_t type, const void *pixels);
static void ring_flush(void);
static void begin_frame(void);

static int vp_x, vp_y, vp_w = SURFACE_W, vp_h = SURFACE_H;
static int sc_x, sc_y, sc_w = SURFACE_W, sc_h = SURFACE_H;
static float depth_near = 0, depth_far = 1;
static float clear_rgba[4], clear_depth = 1;

static int blend, alpha_test, depth_test, cull, scissor, fog;
static float fog_start, fog_end = 1;
static uint32_t fog_color;
static C3D_FogLut fog_lut;
static float fog_key[6];
static uint32_t blend_src = GL_ONE, blend_dst = GL_ZERO;
static uint32_t alpha_func = GL_ALWAYS, depth_func = GL_LESS;
static float alpha_ref;
static int depth_mask = 1, color_mask = GPU_WRITE_COLOR;
static uint32_t cull_mode = GL_BACK, front_face = GL_CCW;

static const mat4 identity_mat = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };

static void mat_identity(mat4 *m)
{
	memset(m, 0, sizeof(*m));
	m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1;
}

static void mat_mul(mat4 *out, const mat4 *a, const mat4 *b)
{
	mat4 r;
	for (int c = 0; c < 4; c++)
		for (int i = 0; i < 4; i++)
			r.m[c * 4 + i] = a->m[i] * b->m[c * 4] + a->m[4 + i] * b->m[c * 4 + 1] +
				a->m[8 + i] * b->m[c * 4 + 2] + a->m[12 + i] * b->m[c * 4 + 3];
	*out = r;
}

static mstack *cur_stack(void)
{
	if (matrix_mode == GL_PROJECTION)
		return &projection;
	if (matrix_mode == GL_TEXTURE)
		return &texmat[active_unit];
	return &modelview;
}

static mat4 *cur_mat(void)
{
	mstack *s = cur_stack();
	return &s->s[s->top];
}

static void mult(const mat4 *m)
{
	mat4 *c = cur_mat();
	mat_mul(c, c, m);
}

void glMatrixMode(uint32_t mode)
{
	matrix_mode = mode;
}

void glLoadIdentity(void)
{
	mat_identity(cur_mat());
}

void glLoadMatrixx(const int32_t *m)
{
	mat4 *c = cur_mat();
	for (int i = 0; i < 16; i++)
		c->m[i] = FX(m[i]);
}

void glLoadMatrixf(const float *m)
{
	memcpy(cur_mat()->m, m, sizeof(float) * 16);
}

void glPushMatrix(void)
{
	mstack *s = cur_stack();
	if (s->top < STACK_DEPTH - 1) {
		s->s[s->top + 1] = s->s[s->top];
		s->top++;
	}
}

void glPopMatrix(void)
{
	mstack *s = cur_stack();
	if (s->top > 0)
		s->top--;
}

void glTranslatex(int32_t x, int32_t y, int32_t z)
{
	mat4 m;
	mat_identity(&m);
	m.m[12] = FX(x);
	m.m[13] = FX(y);
	m.m[14] = FX(z);
	mult(&m);
}

void glScalex(int32_t x, int32_t y, int32_t z)
{
	mat4 m;
	mat_identity(&m);
	m.m[0] = FX(x);
	m.m[5] = FX(y);
	m.m[10] = FX(z);
	mult(&m);
}

void glRotatex(int32_t angle, int32_t ax, int32_t ay, int32_t az)
{
	float x = FX(ax), y = FX(ay), z = FX(az);
	float len = sqrtf(x * x + y * y + z * z);
	if (len == 0)
		return;
	x /= len;
	y /= len;
	z /= len;

	float a = FX(angle) * (float)M_PI / 180.0f;
	float c = cosf(a), s = sinf(a), t = 1 - c;
	mat4 m;
	mat_identity(&m);
	m.m[0] = x * x * t + c;
	m.m[1] = y * x * t + z * s;
	m.m[2] = x * z * t - y * s;
	m.m[4] = x * y * t - z * s;
	m.m[5] = y * y * t + c;
	m.m[6] = y * z * t + x * s;
	m.m[8] = x * z * t + y * s;
	m.m[9] = y * z * t - x * s;
	m.m[10] = z * z * t + c;
	mult(&m);
}

void glOrthox(int32_t l, int32_t r, int32_t b, int32_t t, int32_t n, int32_t f)
{
	float L = FX(l), R = FX(r), B = FX(b), T = FX(t), N = FX(n), F = FX(f);
	mat4 m;
	mat_identity(&m);
	m.m[0] = 2 / (R - L);
	m.m[5] = 2 / (T - B);
	m.m[10] = -2 / (F - N);
	m.m[12] = -(R + L) / (R - L);
	m.m[13] = -(T + B) / (T - B);
	m.m[14] = -(F + N) / (F - N);
	mult(&m);
}

void glFrustumx(int32_t l, int32_t r, int32_t b, int32_t t, int32_t n, int32_t f)
{
	float L = FX(l), R = FX(r), B = FX(b), T = FX(t), N = FX(n), F = FX(f);
	mat4 m;
	memset(&m, 0, sizeof(m));
	m.m[0] = 2 * N / (R - L);
	m.m[5] = 2 * N / (T - B);
	m.m[8] = (R + L) / (R - L);
	m.m[9] = (T + B) / (T - B);
	m.m[10] = -(F + N) / (F - N);
	m.m[11] = -1;
	m.m[14] = -2 * F * N / (F - N);
	mult(&m);
}

static buffer *get_buffer(uint32_t name)
{
	if (name >= nbuffers) {
		uint32_t n = name + 64;
		buffers = realloc(buffers, n * sizeof(*buffers));
		memset(buffers + nbuffers, 0, (n - nbuffers) * sizeof(*buffers));
		nbuffers = n;
	}
	return &buffers[name];
}

void glGenBuffers(int32_t n, uint32_t *names)
{
	while (n-- > 0)
		*names++ = next_buffer++;
}

void glDeleteBuffers(int32_t n, const uint32_t *names)
{
	while (n-- > 0) {
		uint32_t id = *names++;
		if (id && id < nbuffers) {
			free(buffers[id].data);
			buffers[id].data = NULL;
			buffers[id].size = 0;
		}
	}
}

void glBindBuffer(uint32_t target, uint32_t name)
{
	if (target == GL_ARRAY_BUFFER)
		array_buffer = name;
	else
		element_buffer = name;
}

void glBufferData(uint32_t target, int32_t size, const void *data, uint32_t usage)
{
	uint32_t name = target == GL_ARRAY_BUFFER ? array_buffer : element_buffer;
	if (!name)
		return;
	buffer *b = get_buffer(name);
	if (b->size != (uint32_t)size) {
		free(b->data);
		b->data = malloc(size);
		b->size = b->data ? size : 0;
	}
	if (data && b->data)
		memcpy(b->data, data, size);
}

static void set_array(array *a, int size, uint32_t type, int stride, const void *ptr)
{
	a->size = size;
	a->type = type;
	a->stride = stride;
	a->ptr = ptr;
	a->buffer = array_buffer;
}

void glVertexPointer(int32_t size, uint32_t type, int32_t stride, const void *ptr)
{
	set_array(&va_pos, size, type, stride, ptr);
}

void glColorPointer(int32_t size, uint32_t type, int32_t stride, const void *ptr)
{
	set_array(&va_color, size, type, stride, ptr);
}

void glNormalPointer(uint32_t type, int32_t stride, const void *ptr)
{
	set_array(&va_normal, 3, type, stride, ptr);
}

void glTexCoordPointer(int32_t size, uint32_t type, int32_t stride, const void *ptr)
{
	set_array(&va_tc[client_unit], size, type, stride, ptr);
}

static array *client_array(uint32_t cap)
{
	switch (cap) {
	case GL_VERTEX_ARRAY: return &va_pos;
	case GL_COLOR_ARRAY: return &va_color;
	case GL_NORMAL_ARRAY: return &va_normal;
	case GL_TEXTURE_COORD_ARRAY: return &va_tc[client_unit];
	}
	return NULL;
}

void glEnableClientState(uint32_t cap)
{
	array *a = client_array(cap);
	if (a)
		a->enabled = 1;
}

void glDisableClientState(uint32_t cap)
{
	array *a = client_array(cap);
	if (a)
		a->enabled = 0;
}

void glActiveTexture(uint32_t unit)
{
	active_unit = (unit - GL_TEXTURE0) & 1;
}

void glClientActiveTexture(uint32_t unit)
{
	client_unit = (unit - GL_TEXTURE0) & 1;
}

static void set_cap(uint32_t cap, int on)
{
	switch (cap) {
	case GL_TEXTURE_2D: tex_enabled[active_unit] = on; break;
	case GL_BLEND: blend = on; break;
	case GL_ALPHA_TEST: alpha_test = on; break;
	case GL_DEPTH_TEST: depth_test = on; break;
	case GL_CULL_FACE: cull = on; break;
	case GL_SCISSOR_TEST: scissor = on; break;
	case GL_FOG: fog = on; break;
	}
}

void glEnable(uint32_t cap)
{
	set_cap(cap, 1);
}

void glDisable(uint32_t cap)
{
	set_cap(cap, 0);
}

void glColor4x(int32_t r, int32_t g, int32_t b, int32_t a)
{
	cur_color[0] = FX(r);
	cur_color[1] = FX(g);
	cur_color[2] = FX(b);
	cur_color[3] = FX(a);
}

void glBlendFunc(uint32_t src, uint32_t dst)
{
	blend_src = src;
	blend_dst = dst;
}

void glAlphaFuncx(uint32_t func, int32_t ref)
{
	alpha_func = func;
	alpha_ref = FX(ref);
}

void glDepthFunc(uint32_t func)
{
	depth_func = func;
}

void glDepthMask(uint8_t flag)
{
	depth_mask = flag;
}

void glColorMask(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	color_mask = (r ? GPU_WRITE_RED : 0) | (g ? GPU_WRITE_GREEN : 0) |
		(b ? GPU_WRITE_BLUE : 0) | (a ? GPU_WRITE_ALPHA : 0);
}

void glCullFace(uint32_t mode)
{
	cull_mode = mode;
}

void glFrontFace(uint32_t mode)
{
	front_face = mode;
}

void glViewport(int32_t x, int32_t y, int32_t w, int32_t h)
{
	vp_x = x;
	vp_y = y;
	vp_w = w;
	vp_h = h;
}

void glScissor(int32_t x, int32_t y, int32_t w, int32_t h)
{
	sc_x = x;
	sc_y = y;
	sc_w = w;
	sc_h = h;
}

void glDepthRangex(int32_t n, int32_t f)
{
	depth_near = FX(n);
	depth_far = FX(f);
}

SOFTFP void glDepthRangef(float n, float f)
{
	depth_near = n;
	depth_far = f;
}

void glClearColorx(int32_t r, int32_t g, int32_t b, int32_t a)
{
	clear_rgba[0] = FX(r);
	clear_rgba[1] = FX(g);
	clear_rgba[2] = FX(b);
	clear_rgba[3] = FX(a);
}

SOFTFP void glClearColor(float r, float g, float b, float a)
{
	clear_rgba[0] = r;
	clear_rgba[1] = g;
	clear_rgba[2] = b;
	clear_rgba[3] = a;
}

SOFTFP void glClearDepthf(float d)
{
	clear_depth = d;
}

void glPixelStorei(uint32_t pname, int32_t param)
{
	if (pname == GL_UNPACK_ALIGNMENT)
		unpack_align = param;
}

/*
 * The GPU may still be sampling a texture the game just deleted (queued draws,
 * or last frame still rendering), so frees wait until the next frame starts.
 */
#define MAX_GRAVES 128
static C3D_Tex graves[MAX_GRAVES];
static int ngraves;

static void bury_textures(void)
{
	while (ngraves)
		C3D_TexDelete(&graves[--ngraves]);
}

static void retire(C3D_Tex *tex)
{
	tex_gen++;
	tex_bytes -= tex->size;
	if (ngraves < MAX_GRAVES && !port_quitting())
		graves[ngraves++] = *tex;
	else
		C3D_TexDelete(tex);
	memset(tex, 0, sizeof(*tex));
}

static void free_tex(texture *t)
{
	retire(&t->tex);
}

static texture *get_texture(uint32_t name)
{
	if (name >= ntextures) {
		uint32_t n = name + 256;
		textures = realloc(textures, n * sizeof(*textures));
		memset(textures + ntextures, 0, (n - ntextures) * sizeof(*textures));
		ntextures = n;
	}
	texture *t = &textures[name];
	if (!t->alive && !t->min_filter) {
		t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
		t->mag_filter = GL_LINEAR;
		t->wrap_s = t->wrap_t = GL_REPEAT;
	}
	return t;
}

void glGenTextures(int32_t n, uint32_t *names)
{
	while (n-- > 0)
		*names++ = next_texture++;
}

void glDeleteTextures(int32_t n, const uint32_t *names)
{
	while (n-- > 0) {
		uint32_t id = *names++;
		if (id == bg_tex)
			bg_tex = 0;
		if (id && id < ntextures && textures[id].alive) {
			free_tex(&textures[id]);
			memset(&textures[id], 0, sizeof(texture));
		}
	}
}

void glBindTexture(uint32_t target, uint32_t name)
{
	bound[active_unit] = name;
}

static GPU_TEXTURE_WRAP_PARAM pica_wrap(uint32_t w)
{
	switch (w) {
	case GL_CLAMP_TO_EDGE: return GPU_CLAMP_TO_EDGE;
	case GL_MIRRORED_REPEAT: return GPU_MIRRORED_REPEAT;
	}
	return GPU_REPEAT;
}

static void apply_params(texture *t)
{
	tex_gen++;
	if (!t->alive)
		return;
	int min_linear = t->min_filter == GL_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_NEAREST ||
		t->min_filter == GL_LINEAR_MIPMAP_LINEAR;
	int mip_linear = t->min_filter == GL_NEAREST_MIPMAP_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_LINEAR;

	C3D_TexSetFilter(&t->tex, t->mag_filter == GL_NEAREST ? GPU_NEAREST : GPU_LINEAR,
		min_linear ? GPU_LINEAR : GPU_NEAREST);
	C3D_TexSetFilterMipmap(&t->tex, mip_linear ? GPU_LINEAR : GPU_NEAREST);
	C3D_TexSetWrap(&t->tex, pica_wrap(t->wrap_s), pica_wrap(t->wrap_t));
}

void glTexParameteri(uint32_t target, uint32_t pname, int32_t param)
{
	texture *t = get_texture(bound[active_unit]);

	switch (pname) {
	case GL_TEXTURE_MIN_FILTER: t->min_filter = param; break;
	case GL_TEXTURE_MAG_FILTER: t->mag_filter = param; break;
	case GL_TEXTURE_WRAP_S: t->wrap_s = param; break;
	case GL_TEXTURE_WRAP_T: t->wrap_t = param; break;
	default: return;
	}
	apply_params(t);
}

void glTexParameterx(uint32_t target, uint32_t pname, int32_t param)
{
	glTexParameteri(target, pname, param);
}

typedef void (*texel_fn)(uint8_t *d, const uint8_t *s);

typedef struct {
	GPU_TEXCOLOR fmt;
	int src_bpp;
	int dst_bpp;
	texel_fn put;
} texfmt;

/* GL byte order is RGBA, the PICA reads texels as little endian words so everything is reversed */
static void tx_rgba8(uint8_t *d, const uint8_t *s)
{
	d[0] = s[3];
	d[1] = s[2];
	d[2] = s[1];
	d[3] = s[0];
}

static void tx_rgb8(uint8_t *d, const uint8_t *s)
{
	d[0] = s[2];
	d[1] = s[1];
	d[2] = s[0];
}

static void tx_rgb565(uint8_t *d, const uint8_t *s)
{
	uint16_t v = (s[0] >> 3) << 11 | (s[1] >> 2) << 5 | s[2] >> 3;
	d[0] = v;
	d[1] = v >> 8;
}

static void tx_16(uint8_t *d, const uint8_t *s)
{
	d[0] = s[0];
	d[1] = s[1];
}

static void tx_la8(uint8_t *d, const uint8_t *s)
{
	d[0] = s[1];
	d[1] = s[0];
}

static void tx_8(uint8_t *d, const uint8_t *s)
{
	d[0] = s[0];
}

static int pick_format(uint32_t format, uint32_t type, texfmt *f)
{
	switch (type) {
	case GL_UNSIGNED_BYTE:
		switch (format) {
		case GL_RGBA: *f = (texfmt){ GPU_RGBA8, 4, 4, tx_rgba8 }; return 0;
		case GL_RGB:
			if (tex_rgb565)
				*f = (texfmt){ GPU_RGB565, 3, 2, tx_rgb565 };
			else
				*f = (texfmt){ GPU_RGB8, 3, 3, tx_rgb8 };
			return 0;
		case GL_LUMINANCE_ALPHA: *f = (texfmt){ GPU_LA8, 2, 2, tx_la8 }; return 0;
		case GL_LUMINANCE: *f = (texfmt){ GPU_L8, 1, 1, tx_8 }; return 0;
		case GL_ALPHA: *f = (texfmt){ GPU_A8, 1, 1, tx_8 }; return 0;
		}
		break;
	case GL_UNSIGNED_SHORT_4_4_4_4: *f = (texfmt){ GPU_RGBA4, 2, 2, tx_16 }; return 0;
	case GL_UNSIGNED_SHORT_5_5_5_1: *f = (texfmt){ GPU_RGBA5551, 2, 2, tx_16 }; return 0;
	case GL_UNSIGNED_SHORT_5_6_5: *f = (texfmt){ GPU_RGB565, 2, 2, tx_16 }; return 0;
	}
	return -1;
}

/*
 * PICA textures are 8x8 tiles in row order, Z-order (x in the even bits) inside each tile.
 * Row 0 in memory is the top of the image, GL's row 0 is the bottom, hence the flip in blit().
 */
static inline uint32_t tiled(uint32_t x, uint32_t y, uint32_t w)
{
	uint32_t tile = (y >> 3) * (w >> 3) + (x >> 3);
	uint32_t m = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
	return tile * 64 + m;
}

/* x part of the Z-order offset inside an 8x8 tile, y adds (y&1)<<1 | (y&2)<<2 | (y&4)<<3 */
static const uint8_t tile_x[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };

/* the common case, a whole level at its own size, without a call per texel */
static void blit_fast(uint8_t *dst, int dw, int dh, int w, int h, const uint8_t *src, const texfmt *f)
{
	int pitch = (w * f->src_bpp + unpack_align - 1) & ~(unpack_align - 1);

	for (int y = 0; y < h; y++) {
		const uint8_t *s = src + y * pitch;
		uint32_t ty = dh - 1 - y;
		uint32_t row = (ty >> 3) * (dw >> 3) * 64 + ((ty & 1) << 1 | (ty & 2) << 2 | (ty & 4) << 3);

		if (f->put == tx_rgb565) {
			for (int x = 0; x < w; x++, s += 3) {
				uint16_t v = (s[0] >> 3) << 11 | (s[1] >> 2) << 5 | s[2] >> 3;
				((uint16_t *)dst)[row + (x >> 3) * 64 + tile_x[x & 7]] = v;
			}
		} else if (f->put == tx_rgba8) {
			for (int x = 0; x < w; x++, s += 4) {
				uint32_t v = s[0] << 24 | s[1] << 16 | s[2] << 8 | s[3];
				((uint32_t *)dst)[row + (x >> 3) * 64 + tile_x[x & 7]] = v;
			}
		} else if (f->put == tx_16) {
			for (int x = 0; x < w; x++, s += 2)
				((uint16_t *)dst)[row + (x >> 3) * 64 + tile_x[x & 7]] = s[0] | s[1] << 8;
		} else {
			for (int x = 0; x < w; x++, s += f->src_bpp)
				f->put(dst + (row + (x >> 3) * 64 + tile_x[x & 7]) * f->dst_bpp, s);
		}
	}
}

/* small mips get blown up to the 8x8 minimum by repeating texels */
static void blit(uint8_t *dst, int dw, int dh, int x0, int y0, int w, int h,
	const uint8_t *src, const texfmt *f)
{
	int pitch = (w * f->src_bpp + unpack_align - 1) & ~(unpack_align - 1);
	int scale_x = dw > w && !x0 ? dw / w : 1;
	int scale_y = dh > h && !y0 ? dh / h : 1;

	if (!x0 && !y0 && w == dw && h == dh) {
		blit_fast(dst, dw, dh, w, h, src, f);
		return;
	}

	for (int y = 0; y < h * scale_y; y++) {
		const uint8_t *row = src + (y / scale_y) * pitch;
		uint32_t ty = dh - 1 - (y0 + y);
		for (int x = 0; x < w * scale_x; x++)
			f->put(dst + tiled(x0 + x, ty, dw) * f->dst_bpp, row + (x / scale_x) * f->src_bpp);
	}
}

static int pot8(int v)
{
	int p = 8;
	while (p < v)
		p <<= 1;
	return p;
}

static int mip_levels(int w, int h)
{
	int n = 0;
	while ((w >> (n + 1)) >= 8 && (h >> (n + 1)) >= 8)
		n++;
	return n;
}

/* VRAM first: whatever stays out of the linear heap is memory the game's heaps get */
static int alloc_tex(texture *t, int w, int h, int levels, GPU_TEXCOLOR fmt, int prefer_vram)
{
	for (int i = 0; i < 4; i++) {
		if (i == 2) {
			if (!ngraves)
				break;
			bury_textures();
		}
		int vram = prefer_vram ^ (i & 1);
		C3D_TexInitParams p = { w, h, levels, fmt, GPU_TEX_2D, vram };
		if (C3D_TexInitWithParams(&t->tex, NULL, p)) {
			t->vram = vram;
			tex_bytes += t->tex.size;
			return 0;
		}
	}
	port_log("out of texture memory (%dx%d, %lu KB in use)\n", w, h, tex_bytes >> 10);
	return -1;
}


/* textures start with level 0 only, the chain is added once the game actually uploads a mip */
static int grow_mips(texture *t)
{
	C3D_Tex old = t->tex;
	int old_vram = t->vram;
	u32 size0;
	void *level0 = C3D_Tex2DGetImagePtr(&old, 0, &size0);

	if (alloc_tex(t, old.width, old.height, mip_levels(old.width, old.height), old.fmt, old_vram)) {
		t->tex = old;
		t->vram = old_vram;
		return -1;
	}
	void *dst = C3D_Tex2DGetImagePtr(&t->tex, 0, NULL);
	if (!old_vram && !t->vram) {
		memcpy(dst, level0, size0);
	} else {
		ring_flush();
		C3D_SyncTextureCopy(level0, 0, dst, 0, size0, 8);
		if (!t->vram)
			GSPGPU_InvalidateDataCache(dst, size0);
	}
	retire(&old);
	apply_params(t);
	t->last_use = 0;
	return 0;
}

/* VRAM isn't CPU friendly, so those levels get built in linear memory and DMA'd over */
/* used this frame or last, so the GPU may still read it: move it to fresh memory before writing */
static void own_copy(texture *t)
{
	if (!t->last_use || t->last_use < frame_no || port_quitting())
		return;

	C3D_Tex old = t->tex;
	int old_vram = t->vram;
	tex_copies++;
	if (alloc_tex(t, old.width, old.height, old.maxLevel, old.fmt, old_vram)) {
		t->tex = old;
		t->vram = old_vram;
		return;
	}
	if (!old_vram && !t->vram) {
		memcpy(t->tex.data, old.data, old.size);
		C3D_TexFlush(&t->tex);
	} else {
		ring_flush();
		C3D_SyncTextureCopy(old.data, 0, t->tex.data, 0, old.size, 8);
		if (!t->vram)
			GSPGPU_InvalidateDataCache(t->tex.data, old.size);
	}
	retire(&old);
	apply_params(t);
	t->last_use = 0;
}

static void upload(texture *t, int level, int x, int y, int w, int h, const void *pixels, const texfmt *f)
{
	own_copy(t);
	tex_gen++;
	int lw = t->tex.width >> level, lh = t->tex.height >> level;
	u32 size;
	uint8_t *dst = C3D_Tex2DGetImagePtr(&t->tex, level, &size);

	if (!t->vram) {
		if (pixels)
			blit(dst, lw, lh, x, y, w, h, pixels, f);
		else
			memset(dst, 0, size);
		C3D_TexFlush(&t->tex);
		return;
	}

	if (port_quitting())
		return;
	uint8_t *stage = linearAlloc(size);
	if (!stage) {
		port_log("no staging memory for a %dx%d texture\n", lw, lh);
		return;
	}
	if (pixels && (x || y || w < lw || h < lh)) {
		ring_flush();
		C3D_SyncTextureCopy((u32 *)dst, 0, (u32 *)stage, 0, size, 8);
		GSPGPU_InvalidateDataCache(stage, size);
	}
	if (pixels)
		blit(stage, lw, lh, x, y, w, h, pixels, f);
	else
		memset(stage, 0, size);
	GSPGPU_FlushDataCache(stage, size);
	ring_flush();
	C3D_TexLoadImage(&t->tex, stage, GPU_TEXFACE_2D, level);
	linearFree(stage);
}

void glTexImage2D(uint32_t target, int32_t level, int32_t ifmt, int32_t w, int32_t h,
	int32_t border, uint32_t format, uint32_t type, const void *pixels)
{
	u64 t = svcGetSystemTick();
	tex_image(level, w, h, format, type, pixels);
	tex_ticks += svcGetSystemTick() - t;
}

/* same key as the texture dump tool: FNV-1a over the first 4 KB of the upload, xor the size */
static uint32_t tex_hash(const uint8_t *p, uint32_t len, int w, int h)
{
	uint32_t hash = 0x811c9dc5;

	if (len > 4096)
		len = 4096;
	while (len--)
		hash = (hash ^ *p++) * 0x01000193;
	return hash ^ (w << 16 | h);
}

static void tex_identify(texture *t, uint32_t name, const void *pixels, const texfmt *f, int w, int h)
{
	t->hidden = 0;
	if (bg_tex == name)
		bg_tex = 0;
	if (!pixels || (!nhide && !bottom_ui))
		return;

	uint32_t hash = tex_hash(pixels, w * h * f->src_bpp, w, h);
	t->hash = hash;
	for (int i = 0; i < nhide; i++)
		if (hide_hash[i] == hash)
			t->hidden = 1;
	if (hash == BG_HASH)
		bg_tex = name;
}

static int draws_hidden(void)
{
	for (int i = 0; i < 2; i++)
		if (tex_enabled[i] && bound[i] < ntextures && textures[bound[i]].hidden)
			return 1;
	return 0;
}

static void tex_image(int32_t level, int32_t w, int32_t h, uint32_t format, uint32_t type, const void *pixels)
{
	texture *t = get_texture(bound[active_unit]);
	texfmt f;

	if (pick_format(format, type, &f)) {
		port_log("glTexImage2D: format %lx type %lx\n", format, type);
		return;
	}

	if (level == 0) {
		int pw = pot8(w), ph = pot8(h);
		if (!t->alive || t->tex.width != pw || t->tex.height != ph || t->tex.fmt != f.fmt) {
			if (t->alive)
				free_tex(t);
			t->alive = !alloc_tex(t, pw, ph, 0, f.fmt, 1);
			if (!t->alive)
				return;
			t->last_use = 0;
		}
		t->w = w;
		t->h = h;
		tex_identify(t, bound[active_unit], pixels, &f, w, h);
		apply_params(t);
	} else if (!t->alive || !use_mipmaps || level > mip_levels(t->tex.width, t->tex.height)) {
		return;
	} else if (level > t->tex.maxLevel && grow_mips(t)) {
		return;
	}

	upload(t, level, 0, 0, w, h, pixels, &f);
}

void glTexSubImage2D(uint32_t target, int32_t level, int32_t x, int32_t y, int32_t w, int32_t h,
	uint32_t format, uint32_t type, const void *pixels)
{
	texture *t = get_texture(bound[active_unit]);
	texfmt f;

	if (!t->alive || level > t->tex.maxLevel || pick_format(format, type, &f) || f.fmt != t->tex.fmt)
		return;

	int lw = t->tex.width >> level, lh = t->tex.height >> level;
	if (x + w > lw || y + h > lh)
		return;
	upload(t, level, x, y, w, h, pixels, &f);
}

void glTexEnvx(uint32_t target, uint32_t pname, int32_t param)
{
	texenv *e = &env[active_unit];

	switch (pname) {
	case GL_TEXTURE_ENV_MODE: e->mode = param; break;
	case GL_COMBINE_RGB: e->combine_rgb = param; break;
	case GL_COMBINE_ALPHA: e->combine_alpha = param; break;
	case GL_SRC0_RGB: case GL_SRC1_RGB: case GL_SRC2_RGB:
		e->src_rgb[pname - GL_SRC0_RGB] = param;
		break;
	case GL_SRC0_ALPHA: case GL_SRC1_ALPHA: case GL_SRC2_ALPHA:
		e->src_alpha[pname - GL_SRC0_ALPHA] = param;
		break;
	case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB:
		e->op_rgb[pname - GL_OPERAND0_RGB] = param;
		break;
	case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA:
		e->op_alpha[pname - GL_OPERAND0_ALPHA] = param;
		break;
	case GL_RGB_SCALE:
		e->scale_rgb = param >= 0x10000 ? param >> 16 : param;
		break;
	case GL_ALPHA_SCALE:
		e->scale_alpha = param >= 0x10000 ? param >> 16 : param;
		break;
	}
}

static GPU_TEVSRC tev_src(uint32_t src, int unit, int first)
{
	switch (src) {
	case GL_TEXTURE: return GPU_TEXTURE0 + unit;
	case GL_TEXTURE0: return GPU_TEXTURE0;
	case GL_TEXTURE1: return GPU_TEXTURE1;
	case GL_CONSTANT: return GPU_CONSTANT;
	case GL_PRIMARY_COLOR: return GPU_PRIMARY_COLOR;
	}
	return first ? GPU_PRIMARY_COLOR : GPU_PREVIOUS;
}

static GPU_COMBINEFUNC tev_func(uint32_t f)
{
	switch (f) {
	case GL_REPLACE: return GPU_REPLACE;
	case GL_ADD: return GPU_ADD;
	case GL_ADD_SIGNED: return GPU_ADD_SIGNED;
	case GL_INTERPOLATE: return GPU_INTERPOLATE;
	case GL_SUBTRACT: return GPU_SUBTRACT;
	case GL_DOT3_RGB: return GPU_DOT3_RGB;
	case GL_DOT3_RGBA: return GPU_DOT3_RGBA;
	}
	return GPU_MODULATE;
}

static GPU_TEVSCALE tev_scale(int s)
{
	return s == 4 ? GPU_TEVSCALE_4 : s == 2 ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1;
}

static void setup_stage(int stage, int unit, const texenv *e)
{
	C3D_TexEnv *s = C3D_GetTexEnv(stage);
	GPU_TEVSRC tex = GPU_TEXTURE0 + unit;
	GPU_TEVSRC prev = stage ? GPU_PREVIOUS : GPU_PRIMARY_COLOR;

	C3D_TexEnvInit(s);
	switch (e->mode) {
	case GL_REPLACE:
		C3D_TexEnvSrc(s, C3D_Both, tex, 0, 0);
		C3D_TexEnvFunc(s, C3D_Both, GPU_REPLACE);
		break;
	case GL_DECAL:
		C3D_TexEnvSrc(s, C3D_RGB, tex, prev, tex);
		C3D_TexEnvOpRgb(s, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
		C3D_TexEnvFunc(s, C3D_RGB, GPU_INTERPOLATE);
		C3D_TexEnvSrc(s, C3D_Alpha, prev, 0, 0);
		C3D_TexEnvFunc(s, C3D_Alpha, GPU_REPLACE);
		break;
	case GL_BLEND:
		C3D_TexEnvSrc(s, C3D_RGB, GPU_CONSTANT, prev, tex);
		C3D_TexEnvFunc(s, C3D_RGB, GPU_INTERPOLATE);
		C3D_TexEnvSrc(s, C3D_Alpha, prev, tex, 0);
		C3D_TexEnvFunc(s, C3D_Alpha, GPU_MODULATE);
		C3D_TexEnvColor(s, e->color);
		break;
	case GL_ADD:
		C3D_TexEnvSrc(s, C3D_RGB, prev, tex, 0);
		C3D_TexEnvFunc(s, C3D_RGB, GPU_ADD);
		C3D_TexEnvSrc(s, C3D_Alpha, prev, tex, 0);
		C3D_TexEnvFunc(s, C3D_Alpha, GPU_MODULATE);
		break;
	case GL_COMBINE: {
		int first = stage == 0;
		C3D_TexEnvSrc(s, C3D_RGB, tev_src(e->src_rgb[0], unit, first),
			tev_src(e->src_rgb[1], unit, first), tev_src(e->src_rgb[2], unit, first));
		C3D_TexEnvSrc(s, C3D_Alpha, tev_src(e->src_alpha[0], unit, first),
			tev_src(e->src_alpha[1], unit, first), tev_src(e->src_alpha[2], unit, first));
		/* GL operand enums are 0x300..0x303, same order as the PICA ones */
		C3D_TexEnvOpRgb(s, e->op_rgb[0] - GL_SRC_COLOR, e->op_rgb[1] - GL_SRC_COLOR, e->op_rgb[2] - GL_SRC_COLOR);
		C3D_TexEnvOpAlpha(s, e->op_alpha[0] - GL_SRC_ALPHA, e->op_alpha[1] - GL_SRC_ALPHA,
			e->op_alpha[2] - GL_SRC_ALPHA);
		C3D_TexEnvFunc(s, C3D_RGB, tev_func(e->combine_rgb));
		C3D_TexEnvFunc(s, C3D_Alpha, tev_func(e->combine_alpha));
		C3D_TexEnvScale(s, C3D_RGB, tev_scale(e->scale_rgb));
		C3D_TexEnvScale(s, C3D_Alpha, tev_scale(e->scale_alpha));
		C3D_TexEnvColor(s, e->color);
		break;
	}
	default:
		C3D_TexEnvSrc(s, C3D_Both, prev, tex, 0);
		C3D_TexEnvFunc(s, C3D_Both, GPU_MODULATE);
		break;
	}
}

static void bind_unit(int u, const C3D_Tex *t)
{
	if (unit_gen[u] == tex_gen && !memcmp(&unit_copy[u], t, sizeof(*t)))
		return;
	unit_copy[u] = *t;
	unit_gen[u] = tex_gen;
	C3D_TexBind(u, &unit_copy[u]);
}

/* everything a draw needs, so it can be replayed later on another target */
typedef struct {
	mat4 pm, tm[2];
	C3D_Tex tex[2];
	int tex_on[2];
	texenv env[2];
	int blend, alpha_test, depth_test, depth_mask, color_mask, cull, fog, scissor;
	uint32_t blend_src, blend_dst, alpha_func, depth_func, cull_mode, front_face, fog_color;
	float alpha_ref, depth_near, depth_far, fog_start, fog_end, p10, p11, p14;
	int vp[4], sc[4];
	int clear_writes;
	float clear_depth;
	vertex *v;
	uint16_t *idx;
	int first, count;
	GPU_Primitive_t prim;
} draw_rec;

/*
 * Draws for a target that isn't bound get queued and replayed when it is (or at
 * frame end). In 3D the game flips eyes per object, so drawing as it comes means
 * a target switch, and a framebuffer flush, on almost every draw.
 */
#define MAX_PENDING 256
static draw_rec pending[3][MAX_PENDING];
static int npending[3];
static draw_rec shadow;


static void apply_texturing(const draw_rec *r, const draw_rec *was)
{
	int stage = 0;

	for (int u = 0; u < 2; u++)
		bind_unit(u, r->tex_on[u] ? &r->tex[u] : &white);

	if (was && r->tex_on[0] == was->tex_on[0] && r->tex_on[1] == was->tex_on[1] &&
			(!r->tex_on[0] || !memcmp(&r->env[0], &was->env[0], sizeof(texenv))) &&
			(!r->tex_on[1] || !memcmp(&r->env[1], &was->env[1], sizeof(texenv))))
		return;

	for (int u = 0; u < 2; u++)
		if (r->tex_on[u])
			setup_stage(stage++, u, &r->env[u]);

	if (!stage) {
		C3D_TexEnv *s = C3D_GetTexEnv(stage++);
		C3D_TexEnvInit(s);
		C3D_TexEnvSrc(s, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	}
	while (stage < 2)
		C3D_TexEnvInit(C3D_GetTexEnv(stage++));
}

static GPU_TESTFUNC test_func(uint32_t f)
{
	static const GPU_TESTFUNC map[8] = {
		GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL, GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS
	};
	return map[(f - GL_NEVER) & 7];
}

static GPU_BLENDFACTOR blend_factor(uint32_t f)
{
	switch (f) {
	case GL_ZERO: return GPU_ZERO;
	case GL_ONE: return GPU_ONE;
	case GL_SRC_COLOR: return GPU_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR: return GPU_ONE_MINUS_SRC_COLOR;
	case GL_SRC_ALPHA: return GPU_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA: return GPU_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA: return GPU_ONE_MINUS_DST_ALPHA;
	case GL_DST_COLOR: return GPU_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR: return GPU_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA_SATURATE: return GPU_SRC_ALPHA_SATURATE;
	}
	return GPU_ONE;
}

/* surface rect -> framebuffer rect, both screens are sideways (240x400 top, 240x320 bottom) */
static void map_rect(int x, int y, int w, int h, int mode, int len, int *fx, int *fy, int *fw, int *fh)
{
	float half = mode == 2 ? SURFACE_W : SURFACE_W / 2;
	float ox = mode == 1 ? SURFACE_W / 2 : 0;
	float sx = (x - ox) * len / half, sw = w * len / half;
	float sy = y * 240.0f / SURFACE_H, sh = h * 240.0f / SURFACE_H;

	*fx = sy;
	*fw = sh;
	*fy = len - (sx + sw);
	*fh = sw;
}

static int eye_mode(int x, int w)
{
	if (x >= SURFACE_W / 2)
		return 1;
	if (x + w <= SURFACE_W / 2)
		return 0;
	return 2;
}

static void bind_vertices(vertex *v);
static int vertex_first(const vertex *v);
static void *ring_alloc(uint32_t size);
static void use_eye(int eye, int mode, int x, int y, int w, int h);
static void switch_target(int t);
static void replay(int t);
static void draw_now(int target, const draw_rec *r);
static void upload_mvp(const mat4 *pm);
static void upload_texmtx(int loc, const mat4 *t);

static void overlay_state(void)
{
	shadow_valid = 0;
	upload_mvp(&identity_mat);
	C3D_TexEnv *e = C3D_GetTexEnv(0);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
}

/* bottom screen goes last: clear, the map's grid tile at 2x, then this frame's icons */
static void draw_bottom(void)
{
	static const float q[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
	texture *t = bg_tex < ntextures && textures[bg_tex].alive ? &textures[bg_tex] : NULL;
	vertex *v = ring_alloc(8 * sizeof(vertex));

	if (!v)
		return;
	memset(v, 0, 8 * sizeof(vertex));
	for (int i = 0; i < 4; i++) {
		v[i].pos[0] = v[i + 4].pos[0] = q[i][0];
		v[i].pos[1] = v[i + 4].pos[1] = q[i][1];
		v[i].color[0] = 0x1a;
		v[i].color[1] = 0x32;
		v[i].color[2] = 0x38;
		v[i].color[3] = 0xff;
		v[i + 4].tc0[0] = (q[i][0] + 1) * 320 / 64;
		v[i + 4].tc0[1] = (q[i][1] + 1) * 240 / 64;
		memset(v[i + 4].color, 0xff, 4);
	}

	switch_target(BOTTOM);
	draw_rec r = {
		.vp = { 0, 0, SURFACE_W, SURFACE_H },
		.clear_writes = GPU_WRITE_ALL,
		.clear_depth = 1,
		.v = v,
		.count = 4,
		.prim = GPU_TRIANGLE_STRIP,
	};
	draw_now(BOTTOM, &r);

	if (t) {
		C3D_Tex grid = t->tex;
		C3D_TexSetWrap(&grid, GPU_REPEAT, GPU_REPEAT);
		C3D_TexSetFilter(&grid, GPU_NEAREST, GPU_NEAREST);
		overlay_state();
		upload_texmtx(u_tex0, &identity_mat);
		bind_unit(0, &grid);
		t->last_use = frame_no + 1;
		C3D_TexEnv *e = C3D_GetTexEnv(0);
		C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, 0, 0);
		C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, vertex_first(v + 4), 4);
	}
	replay(BOTTOM);
}

static void begin_frame(void)
{
	if (in_frame)
		return;
	/* once the system wants us closed it may already have the GPU back, waiting on it would hang */
	if (port_quitting())
		return;
	u64 t = svcGetSystemTick();
	gl_stage = "C3D_FrameBegin";
	/* SYNCDRAW always waits for a fresh vblank; we only need one between presents */
	if (C3D_FrameCounter(0) == last_vblank)
		C3D_FrameSync();
	C3D_FrameBegin(0);
	gl_stage = "drawing";
	wait_ticks += svcGetSystemTick() - t;
	in_frame = 1;
	cur_eye = -1;
	npending[0] = npending[1] = npending[2] = 0;
	shadow_valid = 0;
	view_valid = 0;
	bury_textures();
}

static void draw_now(int target, const draw_rec *r);

static void switch_target(int t)
{
	if (t == cur_eye)
		return;
	/*
	 * Binding another target mid-list only invalidates the framebuffer cache, and
	 * even a flush right before it doesn't always land first on hardware: dirty
	 * tiles get dropped and things flicker black or vanish. Ending the command list
	 * here has the GPU finish and flush it before the next target is touched.
	 */
	if (cur_eye >= 0) {
		ring_flush();
		C3D_FrameSplit(0);
		nswitches++;
	}
	C3D_FrameDrawOn(eyes[t]);
	cur_eye = t;
	view_valid = 0;
}

static void replay(int t)
{
	for (int i = 0; i < npending[t]; i++)
		draw_now(t, &pending[t][i]);
	npending[t] = 0;
}

static void select_target(int t)
{
	if (t == cur_eye)
		return;
	switch_target(t);
	replay(t);
}

static void set_view(int eye, int mode, const int *vp, int sc_on, const int *sc)
{
	int fx, fy, fw, fh;
	int len = eye == BOTTOM ? 320 : 400;
	int key[9] = { mode, vp[0], vp[1], vp[2], vp[3], sc_on, sc_on ? sc[0] : 0, sc_on ? sc[1] : 0, 0 };

	key[8] = sc_on ? sc[2] << 16 | sc[3] : 0;
	if (view_valid && !memcmp(key, view_cache, sizeof(key)))
		return;
	memcpy(view_cache, key, sizeof(key));
	view_valid = 1;

	map_rect(vp[0], vp[1], vp[2], vp[3], mode, len, &fx, &fy, &fw, &fh);
	if (fx < 0) { fw += fx; fx = 0; }
	if (fy < 0) { fh += fy; fy = 0; }
	if (fw > 0 && fh > 0)
		C3D_SetViewport(fx, fy, fw, fh);

	if (sc_on) {
		map_rect(sc[0], sc[1], sc[2], sc[3], mode, len, &fx, &fy, &fw, &fh);
		if (fx < 0) { fw += fx; fx = 0; }
		if (fy < 0) { fh += fy; fy = 0; }
		C3D_SetScissor(GPU_SCISSOR_NORMAL, fx, fy, fx + (fw > 0 ? fw : 0), fy + (fh > 0 ? fh : 0));
	} else {
		C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	}
}

static void use_eye(int eye, int mode, int x, int y, int w, int h)
{
	const int vp[4] = { x, y, w, h }, sc[4] = { sc_x, sc_y, sc_w, sc_h };

	select_target(eye);
	set_view(eye, mode, vp, scissor, sc);
}

/*
 * GL clip space -> PICA clip space: z goes from [-w, w] to [-w, 0] and the whole
 * thing is turned 90 degrees to match the sideways framebuffer.
 */
static void upload_mvp(const mat4 *pm)
{
	const float *m = pm->m;

	C3D_FVUnifSet(GPU_VERTEX_SHADER, u_mvp + 0, m[1], m[5], m[9], m[13]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, u_mvp + 1, -m[0], -m[4], -m[8], -m[12]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, u_mvp + 2, 0.5f * (m[2] - m[3]), 0.5f * (m[6] - m[7]),
		0.5f * (m[10] - m[11]), 0.5f * (m[14] - m[15]));
	C3D_FVUnifSet(GPU_VERTEX_SHADER, u_mvp + 3, m[3], m[7], m[11], m[15]);
}

static void upload_texmtx(int loc, const mat4 *t)
{
	const float *m = t->m;

	C3D_FVUnifSet(GPU_VERTEX_SHADER, loc, m[0], m[4], m[8], m[12]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, loc + 1, m[1], m[5], m[9], m[13]);
}

void glFogx(uint32_t pname, int32_t param)
{
	if (pname == GL_FOG_START)
		fog_start = FX(param);
	else if (pname == GL_FOG_END)
		fog_end = FX(param);
}

void glFogxv(uint32_t pname, const int32_t *params)
{
	if (pname != GL_FOG_COLOR)
		return;
	fog_color = 0;
	for (int i = 0; i < 3; i++) {
		float c = FX(params[i]);
		uint32_t v = c <= 0 ? 0 : c >= 1 ? 255 : (uint32_t)(c * 255.0f);
		fog_color |= v << (i * 8);
	}
}

/* fog LUT is indexed by window depth, map each entry back to eye distance for GL linear fog */
static void apply_fog(const draw_rec *r)
{
	float fog_start = r->fog_start, fog_end = r->fog_end, depth_near = r->depth_near, depth_far = r->depth_far;
	float p[16];
	float key[6] = { fog_start, fog_end, r->p10, r->p14, depth_near, depth_far };
	float data[256];

	p[10] = r->p10;
	p[11] = r->p11;
	p[14] = r->p14;
	if (!r->fog) {
		C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
		return;
	}

	if (memcmp(key, fog_key, sizeof(key))) {
		memcpy(fog_key, key, sizeof(key));
		float range = depth_far - depth_near;
		for (int i = 0; i <= 128; i++) {
			float d = i / 128.0f;
			float z = range != 0 ? 2 * (d - depth_near) / range - 1 : 0;
			float dist;
			if (p[11] != 0)
				dist = z + p[10] != 0 ? p[14] / (z + p[10]) : fog_end;
			else
				dist = p[10] != 0 ? -(z - p[14]) / p[10] : fog_end;

			float f = fog_end != fog_start ? (fog_end - dist) / (fog_end - fog_start) : 1;
			f = f < 0 ? 0 : f > 1 ? 1 : f;
			if (i < 128)
				data[i] = f;
			if (i > 0)
				data[i + 127] = f - data[i - 1];
		}
		FogLut_FromArray(&fog_lut, data);
		fog_lut_bound = 0;
	}

	C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
	C3D_FogColor(r->fog_color);
	/* a bind uploads all 128 entries */
	if (!fog_lut_bound) {
		C3D_FogLutBind(&fog_lut);
		fog_lut_bound = 1;
	}
}

static void capture(draw_rec *r)
{
	const float *p = projection.s[projection.top].m;

	mat_mul(&r->pm, &projection.s[projection.top], &modelview.s[modelview.top]);
	r->tm[0] = texmat[0].s[texmat[0].top];
	r->tm[1] = texmat[1].s[texmat[1].top];
	for (int u = 0; u < 2; u++) {
		texture *t = bound[u] < ntextures ? &textures[bound[u]] : NULL;
		r->tex_on[u] = tex_enabled[u] && t && t->alive;
		if (r->tex_on[u]) {
			r->tex[u] = t->tex;
			r->env[u] = env[u];
			t->last_use = frame_no + 1;
		}
	}
	r->blend = blend;
	r->blend_src = blend_src;
	r->blend_dst = blend_dst;
	r->alpha_test = alpha_test;
	r->alpha_func = alpha_func;
	r->alpha_ref = alpha_ref;
	r->depth_test = depth_test;
	r->depth_func = depth_func;
	r->depth_mask = depth_mask;
	r->color_mask = color_mask;
	r->depth_near = depth_near;
	r->depth_far = depth_far;
	r->cull = cull;
	r->cull_mode = cull_mode;
	r->front_face = front_face;
	r->fog = fog;
	r->fog_color = fog_color;
	r->fog_start = fog_start;
	r->fog_end = fog_end;
	r->p10 = p[10];
	r->p11 = p[11];
	r->p14 = p[14];
	r->scissor = scissor;
	r->vp[0] = vp_x; r->vp[1] = vp_y; r->vp[2] = vp_w; r->vp[3] = vp_h;
	r->sc[0] = sc_x; r->sc[1] = sc_y; r->sc[2] = sc_w; r->sc[3] = sc_h;
	r->clear_writes = 0;
}

#define SAME(f) (was && !memcmp(&r->f, &was->f, sizeof(r->f)))

/* every citro3d setter dirties a whole block, so only call the ones whose input changed */
static void apply(const draw_rec *r)
{
	const draw_rec *was = shadow_valid ? &shadow : NULL;

	if (!SAME(pm))
		upload_mvp(&r->pm);
	if (!SAME(tm[0]))
		upload_texmtx(u_tex0, &r->tm[0]);
	if (!SAME(tm[1]))
		upload_texmtx(u_tex1, &r->tm[1]);

	apply_texturing(r, was);

	if (!was || r->fog != was->fog || (r->fog && (!SAME(fog_color) || !SAME(fog_start) || !SAME(fog_end) ||
			!SAME(p10) || !SAME(p11) || !SAME(p14) || !SAME(depth_near) || !SAME(depth_far))))
		apply_fog(r);

	if (!was || r->blend != was->blend || !SAME(blend_src) || !SAME(blend_dst)) {
		if (r->blend)
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, blend_factor(r->blend_src), blend_factor(r->blend_dst),
				blend_factor(r->blend_src), blend_factor(r->blend_dst));
		else
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	}

	if (!SAME(alpha_test) || !SAME(alpha_func) || !SAME(alpha_ref))
		C3D_AlphaTest(r->alpha_test, test_func(r->alpha_func), (int)(r->alpha_ref * 255.0f));

	if (!SAME(depth_test) || !SAME(depth_func) || !SAME(depth_mask) || !SAME(color_mask)) {
		if (r->depth_test)
			C3D_DepthTest(true, test_func(r->depth_func), r->color_mask | (r->depth_mask ? GPU_WRITE_DEPTH : 0));
		else
			C3D_DepthTest(true, GPU_ALWAYS, r->color_mask);
	}

	/* window z = (far - near) * z + far, PICA hands us z in [-1, 0] */
	if (!SAME(depth_near) || !SAME(depth_far))
		C3D_DepthMap(true, r->depth_far - r->depth_near, r->depth_far);

	if (!SAME(cull) || !SAME(cull_mode) || !SAME(front_face)) {
		if (!r->cull || r->cull_mode == GL_FRONT_AND_BACK) {
			C3D_CullFace(GPU_CULL_NONE);
		} else {
			int ccw_is_front = r->front_face == GL_CCW;
			int cull_ccw = (r->cull_mode == GL_FRONT) == ccw_is_front;
			C3D_CullFace(cull_ccw ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW);
		}
	}

	shadow = *r;
	shadow_valid = 1;
}

#undef SAME

/* the GPU only reads the ring once the command list goes out, so flush it then, not per draw */
static uint32_t ring_flushed;

static void ring_flush(void)
{
	if (ring_pos > ring_flushed) {
		GSPGPU_FlushDataCache(ring[ring_idx] + ring_flushed, ring_pos - ring_flushed);
		ring_flushed = ring_pos;
	}
}

static void *ring_alloc(uint32_t size)
{
	/* keep everything vertex aligned, see vertex_first */
	size = (size + 31) & ~31;
	if (ring_pos + size > RING_SIZE) {
		if (!ring_full)
			port_log("vertex ring full\n");
		ring_full = 1;
		return NULL;
	}
	void *p = ring[ring_idx] + ring_pos;
	ring_pos += size;
	return p;
}

static const uint8_t *array_base(const array *a)
{
	if (a->buffer) {
		if (a->buffer >= nbuffers || !buffers[a->buffer].data)
			return NULL;
		return (const uint8_t *)buffers[a->buffer].data + (uintptr_t)a->ptr;
	}
	return a->ptr;
}

static int type_size(uint32_t type)
{
	switch (type) {
	case GL_BYTE:
	case GL_UNSIGNED_BYTE: return 1;
	case GL_SHORT:
	case GL_UNSIGNED_SHORT: return 2;
	}
	return 4;
}

static float read_comp(const uint8_t *p, uint32_t type)
{
	switch (type) {
	case GL_BYTE: return *(const int8_t *)p;
	case GL_UNSIGNED_BYTE: return *p;
	case GL_SHORT: return *(const int16_t *)p;
	case GL_UNSIGNED_SHORT: return *(const uint16_t *)p;
	case GL_FLOAT: return *(const float *)p;
	case GL_FIXED: return FX(*(const int32_t *)p);
	}
	return 0;
}

#define SRC(i) (list ? base + list[i] * stride : base + (first + (i)) * stride)

static void fetch(const array *a, const uint8_t *base, int first, const uint16_t *list, int count,
	float *out, int ncomp, int out_stride)
{
	int n = a->size < ncomp ? a->size : ncomp;
	int csize = type_size(a->type);
	int stride = a->stride ? a->stride : a->size * csize;
	const uint8_t *p;

	if (a->type == GL_FIXED && n == 3) {
		for (int i = 0; i < count; i++, out += out_stride) {
			const int32_t *s = (const int32_t *)SRC(i);
			out[0] = FX(s[0]);
			out[1] = FX(s[1]);
			out[2] = FX(s[2]);
		}
		return;
	}
	if (a->type == GL_SHORT && n == 2) {
		for (int i = 0; i < count; i++, out += out_stride) {
			const int16_t *s = (const int16_t *)SRC(i);
			out[0] = s[0];
			out[1] = s[1];
		}
		return;
	}
	if (a->type == GL_FLOAT) {
		for (int i = 0; i < count; i++, out += out_stride)
			memcpy(out, SRC(i), n * 4);
		return;
	}
	if (a->type == GL_FIXED) {
		for (int i = 0; i < count; i++, out += out_stride) {
			p = SRC(i);
			for (int c = 0; c < n; c++)
				out[c] = FX(((const int32_t *)p)[c]);
		}
		return;
	}
	for (int i = 0; i < count; i++, out += out_stride) {
		p = SRC(i);
		for (int c = 0; c < n; c++)
			out[c] = read_comp(p + c * csize, a->type);
	}
}

static void fetch_color(const uint8_t *base, int first, const uint16_t *list, int count, vertex *v)
{
	const array *a = &va_color;
	int csize = type_size(a->type);
	int stride = a->stride ? a->stride : a->size * csize;
	const uint8_t *p;

	if (a->type == GL_UNSIGNED_BYTE && a->size == 4) {
		for (int i = 0; i < count; i++) {
			p = SRC(i);
			*(uint32_t *)v[i].color = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
		}
		return;
	}
	for (int i = 0; i < count; i++) {
		p = SRC(i);
		if (a->type == GL_UNSIGNED_BYTE) {
			memcpy(v[i].color, p, 4);
			if (a->size == 3)
				v[i].color[3] = 255;
			continue;
		}
		for (int c = 0; c < 4; c++) {
			float f = c < a->size ? read_comp(p + c * csize, a->type) : 1.0f;
			v[i].color[c] = f <= 0 ? 0 : f >= 1 ? 255 : (uint8_t)(f * 255.0f);
		}
	}
}

#undef SRC

/* list, when given, picks the source vertices one by one instead of first..first+count */
static vertex *build_vertices(int first, const uint16_t *list, int count)
{
	vertex *v = ring_alloc(count * sizeof(vertex));
	const uint8_t *base;

	if (!v)
		return NULL;

	base = va_pos.enabled ? array_base(&va_pos) : NULL;
	if (!base)
		return NULL;
	if (va_pos.size < 3 || va_pos.type != GL_FIXED)
		for (int i = 0; i < count; i++)
			v[i].pos[2] = 0;
	fetch(&va_pos, base, first, list, count, v->pos, 3, sizeof(vertex) / 4);

	for (int u = 0; u < 2; u++) {
		float *tc = u ? v->tc1 : v->tc0;
		base = va_tc[u].enabled && tex_enabled[u] ? array_base(&va_tc[u]) : NULL;
		if (base) {
			fetch(&va_tc[u], base, first, list, count, tc, 2, sizeof(vertex) / 4);
		} else {
			for (int i = 0; i < count; i++)
				(u ? v[i].tc1 : v[i].tc0)[0] = (u ? v[i].tc1 : v[i].tc0)[1] = 0;
		}
	}

	base = va_color.enabled ? array_base(&va_color) : NULL;
	if (base) {
		fetch_color(base, first, list, count, v);
	} else {
		uint8_t c[4];
		for (int i = 0; i < 4; i++)
			c[i] = cur_color[i] <= 0 ? 0 : cur_color[i] >= 1 ? 255 : (uint8_t)(cur_color[i] * 255.0f);
		for (int i = 0; i < count; i++)
			memcpy(v[i].color, c, 4);
	}

	return v;
}

static void bind_vertices(vertex *v)
{
	C3D_BufInfo *bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, v, sizeof(vertex), 4, 0x3210);
	vbase = v;
}

/* bind the whole ring once and draw at offsets, a BufInfo change resends all 12 attribute buffers */
static int vertex_first(const vertex *v)
{
	vertex *base = (vertex *)ring[ring_idx];

	if (vbase != base)
		bind_vertices(base);
	return v - base;
}

static GPU_Primitive_t pica_prim(uint32_t mode)
{
	switch (mode) {
	case GL_TRIANGLE_STRIP: return GPU_TRIANGLE_STRIP;
	case GL_TRIANGLE_FAN: return GPU_TRIANGLE_FAN;
	}
	return GPU_TRIANGLES;
}

/* 3D off in the game means one full-width view; a half-width viewport means it's doing eyes again */
static int wants_eye(int mode, int eye)
{
	if (mode != 2) {
		stereo_seen = 1;
		return mode == eye;
	}
	return stereo || eye == 0;
}

static void apply_clear(const draw_rec *r)
{
	shadow_valid = 0;
	upload_mvp(&identity_mat);

	C3D_TexEnv *s = C3D_GetTexEnv(0);
	C3D_TexEnvInit(s);
	C3D_TexEnvSrc(s, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvInit(C3D_GetTexEnv(1));

	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(true, GPU_ALWAYS, r->clear_writes);
	C3D_DepthMap(true, 0.0f, r->clear_depth);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
}

static void draw_now(int target, const draw_rec *r)
{
	if (trace_gpu)
		port_log("  %s on %d, %d verts, vp %d,%d %dx%d\n", r->clear_writes ? "clear" : r->idx ? "elements" : "arrays",
			target, r->count, r->vp[0], r->vp[1], r->vp[2], r->vp[3]);
	set_view(target, eye_mode(r->vp[0], r->vp[2]), r->vp, r->scissor, r->sc);
	if (r->clear_writes) {
		apply_clear(r);
		if (!skip_clears)
			C3D_DrawArrays(r->prim, vertex_first(r->v), r->count);
		return;
	}
	apply(r);
	if (skip_draws)
		return;
	if (r->idx) {
		vertex_first(r->v);
		C3D_DrawElements(r->prim, r->count, C3D_UNSIGNED_SHORT, r->idx);
	} else {
		C3D_DrawArrays(r->prim, vertex_first(r->v), r->count);
	}
}

static void deliver(int t, const draw_rec *r)
{
	if (cur_eye < 0)
		select_target(t);
	if (t == cur_eye) {
		draw_now(t, r);
	} else if (npending[t] < MAX_PENDING) {
		pending[t][npending[t]++] = *r;
	} else {
		select_target(t);
		draw_now(t, r);
	}
}

static void route(const draw_rec *r, int below)
{
	int mode = eye_mode(r->vp[0], r->vp[2]);
	unsigned targets = 0;

	for (int eye = 0; eye < 2; eye++)
		if (wants_eye(mode, eye))
			targets |= below ? (eye ? 0 : 1u << BOTTOM) : 1u << eye;
	if (!targets)
		return;

	if (cur_eye >= 0 && (targets & 1u << cur_eye)) {
		draw_now(cur_eye, r);
		targets &= ~(1u << cur_eye);
	}
	while (targets) {
		int t = __builtin_ctz(targets);
		targets &= ~(1u << t);
		deliver(t, r);
	}
}

/* does this draw sit inside one of the icons the game drew this frame, in its 480x320 touch space */
static int on_icon(const vertex *v, int count)
{
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, w0 = 0;
	mat4 pm;

	if (!nicons || count > 64)
		return 0;
	mat_mul(&pm, &projection.s[projection.top], &modelview.s[modelview.top]);
	const float *m = pm.m;
	for (int i = 0; i < count; i++) {
		const float *p = v[i].pos;
		float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
		if (w <= 0)
			return 0;
		/* icons sit flat at one depth, world geometry like Leon's floor shadow doesn't */
		if (i == 0)
			w0 = w;
		else if (fabsf(w - w0) > w0 * 0.001f)
			return 0;
		float x = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / w;
		float y = (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / w;
		x = (x + 1) * (SURFACE_W / 2);
		y = (1 - y) * (SURFACE_H / 2);
		if (x < x0) x0 = x;
		if (x > x1) x1 = x;
		if (y < y0) y0 = y;
		if (y > y1) y1 = y;
	}

	float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
	for (int i = 0; i < nicons; i++) {
		if (cx < icons[i].x || cx > icons[i].x + icons[i].w || cy < icons[i].y || cy > icons[i].y + icons[i].h)
			continue;
		/* pad takes anything on it; a button only its own graphic, so ammo/health over the weapon icon stay up */
		float bw = x1 - x0, bh = y1 - y0;
		if (icons[i].whole ? bw <= icons[i].w * 2 && bh <= icons[i].h * 2 :
				bw >= icons[i].w * 0.6f && bh >= icons[i].h * 0.6f &&
				bw <= icons[i].w * 1.5f && bh <= icons[i].h * 1.5f)
			return 1;
	}
	return 0;
}

void gl_icon_rect(int x, int y, int w, int h, int whole)
{
	if (bottom_ui && nicons < MAX_ICONS && w > 0 && h > 0)
		icons[nicons++] = (typeof(icons[0])){ x, y, w, h, whole };
}

int gl_bottom_ui(void)
{
	return bottom_ui;
}

static void draw_ticks(u64 t0, u64 t1, u64 t2)
{
	build_ticks += t1 - t0;
	state_ticks += t2 - t1;
	submit_ticks += svcGetSystemTick() - t2;
	ndraws++;
}

static void draw_arrays(uint32_t mode, int32_t first, int32_t count)
{
	if (suppress || port_quitting() || draws_hidden())
		return;
	if (count < 3 || mode < GL_TRIANGLES)
		return;

	begin_frame();
	u64 t0 = svcGetSystemTick();
	vertex *v = build_vertices(first, NULL, count);
	if (!v)
		return;
	int below = on_icon(v, count);
	u64 t1 = svcGetSystemTick();
	static draw_rec r;
	capture(&r);
	r.v = v;
	r.idx = NULL;
	r.count = count;
	r.prim = pica_prim(mode);
	u64 t2 = svcGetSystemTick();
	route(&r, below);
	draw_ticks(t0, t1, t2);
}

/* lots of draws use a few triangles out of a big shared array, so only convert what the indices hit */
static uint16_t *seen_slot;
static uint16_t *seen_stamp;
static uint16_t stamp_now;
static uint16_t pick[65536];

static int compact(const uint16_t *in, int count, uint16_t *out)
{
	int n = 0;

	if (!seen_slot) {
		seen_slot = malloc(65536 * sizeof(uint16_t));
		seen_stamp = calloc(65536, sizeof(uint16_t));
		if (!seen_slot || !seen_stamp)
			return -1;
	}
	/* the stamp tells "seen in this draw" apart from earlier ones without clearing the table */
	if (++stamp_now == 0) {
		memset(seen_stamp, 0, 65536 * sizeof(uint16_t));
		stamp_now = 1;
	}
	for (int i = 0; i < count; i++) {
		uint16_t k = in[i];
		if (seen_stamp[k] != stamp_now) {
			seen_stamp[k] = stamp_now;
			seen_slot[k] = n;
			pick[n++] = k;
		}
		out[i] = seen_slot[k];
	}
	return n;
}

static void draw_elements(uint32_t mode, int32_t count, uint32_t type, const void *indices)
{
	if (suppress || port_quitting() || draws_hidden())
		return;
	const uint8_t *src = indices;
	uint32_t lo = 0xffff, hi = 0;

	if (count < 3 || mode < GL_TRIANGLES)
		return;
	if (element_buffer) {
		if (element_buffer >= nbuffers || !buffers[element_buffer].data)
			return;
		src = (const uint8_t *)buffers[element_buffer].data + (uintptr_t)indices;
	}

	begin_frame();
	uint16_t *idx = ring_alloc(count * 2);
	if (!idx)
		return;
	u64 t0 = svcGetSystemTick();

	if (type == GL_UNSIGNED_BYTE) {
		for (int i = 0; i < count; i++) {
			idx[i] = src[i];
			if (src[i] < lo) lo = src[i];
			if (src[i] > hi) hi = src[i];
		}
	} else {
		const uint16_t *s = (const uint16_t *)src;
		for (int i = 0; i < count; i++) {
			if (s[i] < lo) lo = s[i];
			if (s[i] > hi) hi = s[i];
		}
		memcpy(idx, s, count * 2);
	}

	vertex *v;
	int nverts = hi - lo + 1;
	if (nverts > count && (nverts = compact(idx, count, idx)) > 0) {
		v = build_vertices(0, pick, nverts);
	} else {
		nverts = hi - lo + 1;
		for (int i = 0; i < count; i++)
			idx[i] -= lo;
		v = build_vertices(lo, NULL, nverts);
	}
	if (!v)
		return;
	int base = v - (vertex *)ring[ring_idx];
	for (int i = 0; i < count; i++)
		idx[i] += base;
	int below = on_icon(v, nverts);
	u64 t1 = svcGetSystemTick();
	static draw_rec r;
	capture(&r);
	r.v = v;
	r.idx = idx;
	r.count = count;
	r.prim = pica_prim(mode);
	u64 t2 = svcGetSystemTick();
	route(&r, below);
	draw_ticks(t0, t1, t2);
}

static void clear_rect(int eye, int x, int y, int w, int h, int mask)
{
	draw_rec r;
	int writes = 0;

	if (mask & GL_COLOR_BUFFER_BIT)
		writes |= color_mask;
	if ((mask & GL_DEPTH_BUFFER_BIT) && depth_mask)
		writes |= GPU_WRITE_DEPTH;
	if (!writes)
		return;

	static const float quad[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
	vertex *v = ring_alloc(4 * sizeof(vertex));
	if (!v)
		return;
	memset(v, 0, 4 * sizeof(vertex));
	for (int i = 0; i < 4; i++) {
		v[i].pos[0] = quad[i][0];
		v[i].pos[1] = quad[i][1];
		for (int c = 0; c < 4; c++)
			v[i].color[c] = clear_rgba[c] <= 0 ? 0 : clear_rgba[c] >= 1 ? 255 : (uint8_t)(clear_rgba[c] * 255.0f);
	}

	r.vp[0] = x; r.vp[1] = y; r.vp[2] = w; r.vp[3] = h;
	r.scissor = 0;
	r.clear_writes = writes;
	r.clear_depth = clear_depth;
	r.v = v;
	r.idx = NULL;
	r.count = 4;
	r.prim = GPU_TRIANGLE_STRIP;
	deliver(eye, &r);
}

static void clear(uint32_t mask)
{
	if (port_quitting())
		return;
	int sc = scissor;
	int x = 0, y = 0, w = SURFACE_W, h = SURFACE_H;

	begin_frame();
	if (sc) {
		x = sc_x;
		y = sc_y;
		w = sc_w;
		h = sc_h;
	}

	/* the clear quad goes through the viewport, so scissor is redundant here */
	scissor = 0;
	int mode = eye_mode(x, w);
	if (mode == 2 && !sc) {
		clear_rect(0, 0, y, SURFACE_W / 2, h, mask);
		if (stereo)
			clear_rect(1, SURFACE_W / 2, y, SURFACE_W / 2, h, mask);
	} else {
		for (int eye = 0; eye < 2; eye++)
			if (wants_eye(mode, eye))
				clear_rect(eye, x, y, w, h, mask);
	}
	scissor = sc;
}

void glReadPixels(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t format, uint32_t type, void *pixels)
{
	if (pixels && w > 0 && h > 0)
		memset(pixels, 0, w * h * (format == GL_RGBA ? 4 : 3));
}

const char *glGetString(uint32_t name)
{
	switch (name) {
	case GL_VENDOR: return "devkitPro";
	case GL_RENDERER: return "PICA200";
	case GL_VERSION: return "OpenGL ES-CM 1.1";
	}
	return "";
}

void glGetIntegerv(uint32_t pname, int32_t *out)
{
	switch (pname) {
	case GL_MAX_TEXTURE_SIZE: *out = 1024; break;
	case GL_MAX_TEXTURE_UNITS: *out = 2; break;
	case GL_MAX_LIGHTS: *out = 8; break;
	case GL_MAX_VIEWPORT_DIMS: out[0] = out[1] = 1024; break;
	default: *out = 0; break;
	}
}

void glGetBooleanv(uint32_t pname, uint8_t *out)
{
	*out = 0;
}

void glDrawArrays(uint32_t mode, int32_t first, int32_t count)
{
	u64 t = svcGetSystemTick();
	draw_arrays(mode, first, count);
	gl_ticks += svcGetSystemTick() - t;
}

void glDrawElements(uint32_t mode, int32_t count, uint32_t type, const void *indices)
{
	u64 t = svcGetSystemTick();
	draw_elements(mode, count, type, indices);
	gl_ticks += svcGetSystemTick() - t;
}

void glClear(uint32_t mask)
{
	u64 t = svcGetSystemTick();
	clear(mask);
	gl_ticks += svcGetSystemTick() - t;
}

static void overlay_bar(vertex *v, float x0, float y0, float x1, float y1, const uint8_t *rgba)
{
	float nx0 = x0 / (SURFACE_W / 2) - 1, nx1 = x1 / (SURFACE_W / 2) - 1;
	float ny0 = 1 - y0 / (SURFACE_H / 2), ny1 = 1 - y1 / (SURFACE_H / 2);
	const float q[4][2] = { { nx0, ny0 }, { nx1, ny0 }, { nx0, ny1 }, { nx1, ny1 } };

	for (int i = 0; i < 4; i++) {
		v[i].pos[0] = q[i][0];
		v[i].pos[1] = q[i][1];
		v[i].pos[2] = 0;
		memcpy(v[i].color, rgba, 4);
	}
}

static void draw_overlay(void)
{
	int x, y, w, h;
	static const uint8_t rgba[4] = { 255, 200, 40, 255 };
	const float t = 3;

	if (!controls_highlight(&x, &y, &w, &h))
		return;
	/* only the menu's text entries get the frame, icons are tapped on the bottom screen */
	for (int i = 0; i < nicons; i++)
		if (icons[i].x == x && icons[i].y == y && icons[i].w == w && icons[i].h == h)
			return;

	vertex *v = ring_alloc(16 * sizeof(vertex));
	if (!v)
		return;
	memset(v, 0, 16 * sizeof(vertex));
	overlay_bar(v, x - t, y - t, x + w + t, y, rgba);
	overlay_bar(v + 4, x - t, y + h, x + w + t, y + h + t, rgba);
	overlay_bar(v + 8, x - t, y, x, y + h, rgba);
	overlay_bar(v + 12, x + w, y, x + w + t, y + h, rgba);

	int sc = scissor;
	scissor = 0;
	for (int eye = 0; eye < 2; eye++) {
		if (eye && !stereo)
			break;
		use_eye(eye, 2, 0, 0, SURFACE_W, SURFACE_H);
		overlay_state();
		for (int i = 0; i < 4; i++) {
			C3D_DrawArrays(GPU_TRIANGLE_STRIP, vertex_first(v + i * 4), 4);
		}
	}
	scissor = sc;
}

void gl_suppress(int on)
{
	suppress = on;
}

void gl_swap(void)
{
	if (!in_frame)
		begin_frame();
	if (!in_frame)
		return;
	draw_overlay();
	for (int i = 0; i < 2; i++)
		if (npending[i])
			select_target(i);
	if (bottom_ui)
		draw_bottom();
	u64 t = svcGetSystemTick();
	if (trace_gpu) {
		port_log("  frame end\n");
		trace_gpu--;
	}
	gl_stage = "C3D_FrameEnd";
	ring_flush();
	if (C3D_GetCmdBufUsage() > cmdbuf_peak)
		cmdbuf_peak = C3D_GetCmdBufUsage();
	C3D_FrameEnd(0);
	last_vblank = C3D_FrameCounter(0);
	gl_stage = "game";
	end_ticks += svcGetSystemTick() - t;
	in_frame = 0;
	if (stereo != stereo_seen) {
		stereo = stereo_seen;
		gfxSet3D(stereo);
	}
	stereo_seen = 0;
	if (ring_pos > ring_peak)
		ring_peak = ring_pos;
	ring_idx ^= 1;
	ring_pos = 0;
	ring_flushed = 0;
	ring_full = 0;
	nicons = 0;
	frame_no++;
}

/* once a second: where the frame time went, in ms per frame */
void gl_perf(void)
{
	static u64 last;

	if (!perf_log)
		return;
	static uint32_t frames;
	u64 now = svcGetSystemTick();

	frames++;
	if (!last)
		last = now;
	if (now - last < SYSCLOCK_ARM11)
		return;

	float f = frames, ms = (float)CPU_TICKS_PER_MSEC;
	float total = (now - last) / ms / f;
	float wait = wait_ticks / ms / f, gl = gl_ticks / ms / f - wait, end = end_ticks / ms / f;
	uint32_t loud, gens;
	sound_stats(&loud, &gens);
	port_log("%.1f fps: game %.1f, gl %.1f, gpu wait %.1f, present %.1f, tex upload %.1f ms, audio %lu/%lu, tex copies %lu\n",
		1000.0f / total, total - gl - wait - end - tex_ticks / ms / f, gl, wait, end,
		tex_ticks / ms / f, loud, gens, tex_copies);
	port_log("  gl: verts %.1f, state %.1f, submit %.1f ms, %lu draws, %lu target switches per frame, cmdbuf peak %d%%\n",
		build_ticks / ms / f, state_ticks / ms / f, submit_ticks / ms / f,
		(uint32_t)(ndraws / f), (uint32_t)(nswitches / f), (int)(cmdbuf_peak * 100));
	cmdbuf_peak = 0;
	tex_copies = ndraws = nswitches = 0;
	build_ticks = state_ticks = submit_ticks = 0;

	last = now;
	frames = 0;
	gl_ticks = wait_ticks = end_ticks = tex_ticks = 0;
}

void gl_stats(void)
{
	if (!perf_log)
		return;
	struct mallinfo mi = mallinfo();
	port_log("frame %lu: heap %u KB (in use %u KB), textures %lu KB, ring peak %lu KB, linear free %lu KB, vram free %lu KB\n",
		port_frame(), mi.arena >> 10, mi.uordblks >> 10, tex_bytes >> 10, ring_peak >> 10,
		linearSpaceFree() >> 10, vramSpaceFree() >> 10);
}

uint32_t gl_vblanks(void)
{
	return C3D_FrameCounter(0);
}

static void standard_state(void)
{
	C3D_TexEnv *e = C3D_GetTexEnv(0);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

static void standard_attribs(void)
{
	C3D_AttrInfo *ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 3, GPU_UNSIGNED_BYTE, 4);
}

/* [PORT] GpuSelfTest=1: a few known-good frames, each logged first so a hang names the step */
static void selftest(void)
{
	static const int32_t pos[4 * 5] = {
		-0x8000, -0x8000, 0, 0, 0,
		 0x8000, -0x8000, 0, 0, 0,
		 0x8000,  0x8000, 0, 0, 0,
		-0x8000,  0x8000, 0, 0, 0,
	};
	static const uint8_t col[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
	static const int16_t tc[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
	static const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
	static const float quad[6][2] = { { 100, 60 }, { 300, 60 }, { 300, 180 }, { 100, 60 }, { 300, 180 }, { 100, 180 } };
	static uint8_t pixels[8 * 8 * 3];
	DVLB_s *sdvlb;
	shaderProgram_s sprog;
	C3D_Mtx ortho;
	uint32_t tex;

	port_log("selftest 1: plain target clear\n");
	begin_frame();
	C3D_RenderTargetClear(eyes[0], C3D_CLEAR_ALL, 0x204080ff, 0);
	C3D_FrameDrawOn(eyes[0]);
	gl_swap();

	port_log("selftest 2: example shader, float attributes\n");
	sdvlb = DVLB_ParseFile((u32 *)simple_shbin, simple_shbin_size);
	shaderProgramInit(&sprog);
	shaderProgramSetVsh(&sprog, &sdvlb->DVLE[0]);
	begin_frame();
	C3D_FrameDrawOn(eyes[0]);
	C3D_BindProgram(&sprog);
	Mtx_OrthoTilt(&ortho, 0.0f, 400.0f, 0.0f, 240.0f, 0.0f, 1.0f, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, shaderInstanceGetUniformLocation(sprog.vertexShader, "projection"), &ortho);
	C3D_AttrInfo *ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 4);
	float *fv = ring_alloc(6 * 7 * sizeof(float));
	for (int i = 0; i < 6; i++) {
		float *v = fv + i * 7;
		v[0] = quad[i][0]; v[1] = quad[i][1]; v[2] = 0.5f;
		v[3] = 1; v[4] = 0.5f; v[5] = 0; v[6] = 1;
	}
	GSPGPU_FlushDataCache(fv, 6 * 7 * sizeof(float));
	C3D_BufInfo *bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, fv, 7 * sizeof(float), 2, 0x10);
	standard_state();
	C3D_SetViewport(0, 0, 240, 400);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
	gl_swap();

	port_log("selftest 3: example shader, port vertex layout\n");
	begin_frame();
	C3D_FrameDrawOn(eyes[0]);
	standard_attribs();
	vertex *pv = ring_alloc(6 * sizeof(vertex));
	memset(pv, 0, 6 * sizeof(vertex));
	for (int i = 0; i < 6; i++) {
		pv[i].pos[0] = quad[i][0];
		pv[i].pos[1] = quad[i][1];
		pv[i].pos[2] = 0.5f;
		pv[i].tc0[0] = 0.2f;
		pv[i].tc0[1] = 1.0f;
	}
	GSPGPU_FlushDataCache(pv, 6 * sizeof(vertex));
	bind_vertices(pv);
	standard_state();
	C3D_SetViewport(0, 0, 240, 400);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
	gl_swap();

	port_log("selftest 4: port shader, port vertex layout\n");
	begin_frame();
	C3D_FrameDrawOn(eyes[0]);
	C3D_BindProgram(&program);
	upload_mvp(&identity_mat);
	upload_texmtx(u_tex0, &identity_mat);
	upload_texmtx(u_tex1, &identity_mat);
	pv = ring_alloc(6 * sizeof(vertex));
	memset(pv, 0, 6 * sizeof(vertex));
	for (int i = 0; i < 6; i++) {
		pv[i].pos[0] = quad[i][0] / 200.0f - 1.0f;
		pv[i].pos[1] = quad[i][1] / 120.0f - 1.0f;
		pv[i].color[0] = 255;
		pv[i].color[2] = 255;
		pv[i].color[3] = 255;
	}
	GSPGPU_FlushDataCache(pv, 6 * sizeof(vertex));
	bind_vertices(pv);
	standard_state();
	C3D_SetViewport(0, 0, 240, 400);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
	gl_swap();

	port_log("selftest 5: port clear\n");
	glViewport(0, 0, SURFACE_W, SURFACE_H);
	glClearColorx(0x4000, 0x8000, 0x4000, 0x10000);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	gl_swap();

	port_log("selftest 6: textured quad\n");
	memset(pixels, 0xc0, sizeof(pixels));
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 8, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 10, 237, 300);
	glVertexPointer(3, GL_FIXED, 20, pos);
	glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
	glTexCoordPointer(2, GL_SHORT, 0, tc);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
	gl_swap();

	port_log("selftest 7: waiting for the last frame\n");
	begin_frame();
	gl_swap();
	port_log("selftest passed, vblanks %lu\n", gl_vblanks());

	shaderProgramFree(&sprog);
	DVLB_Free(sdvlb);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteTextures(1, &tex);
	next_texture = 1;
	trace_gpu = port_option("Trace", 0) ? 2 : 0;
}

void gl_init(void)
{
	tex_rgb565 = port_option("TextureRGB565", 1);
	use_mipmaps = port_option("Mipmaps", 1);
	skip_draws = port_option("SkipDraws", 0);
	skip_clears = port_option("SkipClears", 0);
	trace_gpu = port_option("Trace", 0) ? 2 : 0;
	perf_log = port_option("Perf", 0);
	bottom_ui = port_option("BottomScreen", 1);

	const char *hide = config_get("PORT", "HideTextures");
	for (const char *p = hide ? hide : "a9544533"; *p && nhide < 16;) {
		char *end;
		hide_hash[nhide++] = strtoul(p, &end, 16);
		p = *end ? end + 1 : end;
	}

	C3D_Init(0x80000);
	gfxSet3D(false);

	for (int i = 0; i < 2; i++) {
		eyes[i] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
		C3D_RenderTargetSetOutput(eyes[i], GFX_TOP, i ? GFX_RIGHT : GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
		ring[i] = linearAlloc(RING_SIZE);
	}
	if (bottom_ui) {
		eyes[BOTTOM] = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
		C3D_RenderTargetSetOutput(eyes[BOTTOM], GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	}

	dvlb = DVLB_ParseFile((u32 *)gles_shbin, gles_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
	C3D_BindProgram(&program);
	u_mvp = shaderInstanceGetUniformLocation(program.vertexShader, "mvp");
	u_tex0 = shaderInstanceGetUniformLocation(program.vertexShader, "texmtx0");
	u_tex1 = shaderInstanceGetUniformLocation(program.vertexShader, "texmtx1");

	standard_attribs();

	C3D_TexInit(&white, 8, 8, GPU_RGBA8);
	memset(white.data, 0xff, white.size);
	C3D_TexFlush(&white);

	mat_identity(&modelview.s[0]);
	mat_identity(&projection.s[0]);
	for (int u = 0; u < 2; u++) {
		mat_identity(&texmat[u].s[0]);
		env[u] = (texenv){
			GL_MODULATE, GL_MODULATE, GL_MODULATE,
			{ GL_TEXTURE, GL_PREVIOUS, GL_CONSTANT }, { GL_TEXTURE, GL_PREVIOUS, GL_CONSTANT },
			{ GL_SRC_COLOR, GL_SRC_COLOR, GL_SRC_ALPHA }, { GL_SRC_ALPHA, GL_SRC_ALPHA, GL_SRC_ALPHA },
			1, 1, 0
		};
	}
	for (int i = 2; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));

	if (port_option("GpuSelfTest", 0))
		selftest();
}
