#ifndef GL_H
#define GL_H

#include <stdint.h>
#include "port.h"

#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_COLOR_BUFFER_BIT 0x4000

#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006

#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_ALWAYS 0x0207

#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308

#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CCW 0x0901

#define GL_CULL_FACE 0x0b44
#define GL_FOG 0x0b60
#define GL_DEPTH_TEST 0x0b71
#define GL_ALPHA_TEST 0x0bc0
#define GL_BLEND 0x0be2
#define GL_SCISSOR_TEST 0x0c11
#define GL_TEXTURE_2D 0x0de1

#define GL_FOG_START 0x0b63
#define GL_FOG_END 0x0b64
#define GL_FOG_COLOR 0x0b66

#define GL_UNPACK_ALIGNMENT 0x0cf5
#define GL_MAX_LIGHTS 0x0d31
#define GL_MAX_TEXTURE_SIZE 0x0d33
#define GL_MAX_VIEWPORT_DIMS 0x0d3a

#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT 0x1406
#define GL_FIXED 0x140c

#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702

#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190a

#define GL_VENDOR 0x1f00
#define GL_RENDERER 0x1f01
#define GL_VERSION 0x1f02

#define GL_ADD 0x0104
#define GL_REPLACE 0x1e01
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_TEXTURE_ENV_MODE 0x2200

#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812f
#define GL_MIRRORED_REPEAT 0x8370

#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_5_6_5 0x8363

#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

#define GL_TEXTURE0 0x84c0
#define GL_TEXTURE1 0x84c1
#define GL_MAX_TEXTURE_UNITS 0x84e2
#define GL_SUBTRACT 0x84e7

#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_RGB_SCALE 0x8573
#define GL_ADD_SIGNED 0x8574
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#define GL_SRC0_RGB 0x8580
#define GL_SRC1_RGB 0x8581
#define GL_SRC2_RGB 0x8582
#define GL_SRC0_ALPHA 0x8588
#define GL_SRC1_ALPHA 0x8589
#define GL_SRC2_ALPHA 0x858a
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND1_RGB 0x8591
#define GL_OPERAND2_RGB 0x8592
#define GL_OPERAND0_ALPHA 0x8598
#define GL_OPERAND1_ALPHA 0x8599
#define GL_OPERAND2_ALPHA 0x859a
#define GL_ALPHA_SCALE 0x0d1c
#define GL_DOT3_RGB 0x86ae
#define GL_DOT3_RGBA 0x86af

#define GL_ARRAY_BUFFER 0x8892

void gl_init(void);
void gl_swap(void);
void gl_stats(void);
void gl_perf(void);
void gl_suppress(int on);
uint32_t gl_vblanks(void);

void glMatrixMode(uint32_t mode);
void glLoadIdentity(void);
void glLoadMatrixx(const int32_t *m);
void glLoadMatrixf(const float *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatex(int32_t x, int32_t y, int32_t z);
void glScalex(int32_t x, int32_t y, int32_t z);
void glRotatex(int32_t angle, int32_t x, int32_t y, int32_t z);
void glOrthox(int32_t l, int32_t r, int32_t b, int32_t t, int32_t n, int32_t f);
void glFrustumx(int32_t l, int32_t r, int32_t b, int32_t t, int32_t n, int32_t f);

void glGenBuffers(int32_t n, uint32_t *names);
void glDeleteBuffers(int32_t n, const uint32_t *names);
void glBindBuffer(uint32_t target, uint32_t name);
void glBufferData(uint32_t target, int32_t size, const void *data, uint32_t usage);

void glVertexPointer(int32_t size, uint32_t type, int32_t stride, const void *ptr);
void glColorPointer(int32_t size, uint32_t type, int32_t stride, const void *ptr);
void glNormalPointer(uint32_t type, int32_t stride, const void *ptr);
void glTexCoordPointer(int32_t size, uint32_t type, int32_t stride, const void *ptr);
void glEnableClientState(uint32_t cap);
void glDisableClientState(uint32_t cap);
void glActiveTexture(uint32_t unit);
void glClientActiveTexture(uint32_t unit);

void glEnable(uint32_t cap);
void glDisable(uint32_t cap);
void glColor4x(int32_t r, int32_t g, int32_t b, int32_t a);
void glBlendFunc(uint32_t src, uint32_t dst);
void glAlphaFuncx(uint32_t func, int32_t ref);
void glDepthFunc(uint32_t func);
void glDepthMask(uint8_t flag);
void glColorMask(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void glCullFace(uint32_t mode);
void glFrontFace(uint32_t mode);
void glViewport(int32_t x, int32_t y, int32_t w, int32_t h);
void glScissor(int32_t x, int32_t y, int32_t w, int32_t h);
void glDepthRangex(int32_t n, int32_t f);
SOFTFP void glDepthRangef(float n, float f);
void glClearColorx(int32_t r, int32_t g, int32_t b, int32_t a);
SOFTFP void glClearColor(float r, float g, float b, float a);
SOFTFP void glClearDepthf(float d);
void glPixelStorei(uint32_t pname, int32_t param);
void glFogx(uint32_t pname, int32_t param);
void glFogxv(uint32_t pname, const int32_t *params);

void glGenTextures(int32_t n, uint32_t *names);
void glDeleteTextures(int32_t n, const uint32_t *names);
void glBindTexture(uint32_t target, uint32_t name);
void glTexParameteri(uint32_t target, uint32_t pname, int32_t param);
void glTexParameterx(uint32_t target, uint32_t pname, int32_t param);
void glTexImage2D(uint32_t target, int32_t level, int32_t ifmt, int32_t w, int32_t h,
	int32_t border, uint32_t format, uint32_t type, const void *pixels);
void glTexSubImage2D(uint32_t target, int32_t level, int32_t x, int32_t y, int32_t w, int32_t h,
	uint32_t format, uint32_t type, const void *pixels);
void glTexEnvx(uint32_t target, uint32_t pname, int32_t param);

void glDrawArrays(uint32_t mode, int32_t first, int32_t count);
void glDrawElements(uint32_t mode, int32_t count, uint32_t type, const void *indices);
void glClear(uint32_t mask);
void glReadPixels(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t format, uint32_t type, void *pixels);

const char *glGetString(uint32_t name);
void glGetIntegerv(uint32_t pname, int32_t *out);
void glGetBooleanv(uint32_t pname, uint8_t *out);

#endif
