#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <stddef.h>

#define DATA_DIR "sdmc:/reDe"

#define SURFACE_W 480
#define SURFACE_H 320

/* the game is built soft-float, we are hard-float */
#define SOFTFP __attribute__((pcs("aapcs")))

typedef int32_t (*s3e_callback)(void *sys, void *user);

void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_exit(int code) __attribute__((noreturn));
void port_pump(void);
int port_quitting(void);
uint32_t port_frame(void);

int load_game(const char *path, void **entry);
void unload_game(void);
uint8_t *game_image(void);
int hook_call(uint8_t *image, uint32_t at, void *fn);
void *import_lookup(const char *name);

void config_load(void);
const char *config_get(const char *group, const char *key);
int port_option(const char *key, int def);

void pointer_poll(void);
void controls_poll(void);
int controls_finger(int *x, int *y);
int controls_highlight(int *x, int *y, int *w, int *h);
void touch_send(uint32_t id, int pressed, int x, int y);
uint32_t auto_keys(void);
void hud_hooks(uint8_t *image);
void gl_icon_rect(int x, int y, int w, int h, int whole);
int gl_bottom_ui(void);

const char *last_import(void);
extern volatile const char *gl_stage;

int lzma_unpack_alone(const uint8_t *src, size_t len, uint8_t **out, size_t *out_len);

#endif
