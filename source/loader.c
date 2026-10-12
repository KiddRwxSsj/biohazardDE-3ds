#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ctype.h>

#include "port.h"

#define LINK_BASE_DEFAULT 0x4a000000
#define VENEER_MAX 16

enum {
	FIX_IMPORTS = 0,
	FIX_RELOCS = 1,
	FIX_SLOTS = 4,
};

typedef struct {
	char magic[4];
	u32 version;
	u16 flags;
	u16 arch;
	u32 fixup_off;
	u32 fixup_size;
	u32 code_off;
	u32 code_size;
	u32 mem_size;
	u32 sig_off;
	u32 sig_size;
	u32 entry;
	u32 unk2c;
	u32 unk30;
	u32 link_base;
	u32 extra_off;
	u32 extra_size;
	u32 unk40;
	u32 unk44;
} xe3u_header;

static int unimplemented(void)
{
	return 0;
}

static u32 rd16(const u8 *p)
{
	return p[0] | p[1] << 8;
}

static u32 rd32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static u8 *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *buf = malloc(*len);
	if (buf && fread(buf, 1, *len, f) != *len) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	return buf;
}

/* svcControlMemory won't give executable pages, ControlProcessMemory on ourselves will (needs Luma) */
/* supervisor mode through svcBackdoor, user mode can't touch the instruction cache */
static s32 flush_icache(void)
{
	__asm__ volatile(
		"mcr p15, 0, %0, c7, c5, 0\n"	/* invalidate the whole I-cache */
		"mcr p15, 0, %0, c7, c5, 6\n"	/* and the branch predictor */
		:: "r"(0));
	return 0;
}

static void *image_base;
static u32 image_size;

static Result protect(void *p, u32 size, u32 perm)
{
	Handle self;
	Result rc = svcDuplicateHandle(&self, CUR_PROCESS_HANDLE);
	if (R_FAILED(rc))
		return rc;
	rc = svcControlProcessMemory(self, (u32)p, (u32)p, size, MEMOP_PROT, perm);
	if (R_SUCCEEDED(rc) && (perm & MEMPERM_EXECUTE))
		rc = svcFlushProcessDataCache(self, (u32)p, size);
	svcCloseHandle(self);
	return rc;
}

static u8 *veneer_base;
static int nveneers;

/* turns a Thumb BL in the game into a BLX to an ARM veneer after the image: ldr pc, [pc, #-4]; .word fn */
int hook_call(u8 *image, u32 at, void *fn)
{
	if (!veneer_base || nveneers == VENEER_MAX)
		return -1;
	u32 *v = (u32 *)(veneer_base + nveneers++ * 8);
	v[0] = 0xe51ff004;
	v[1] = (u32)fn;

	/* BLX imm: target = Align(pc + 4, 4) + offset, offset split 11:11 bits, low bit of H2 clear */
	s32 off = (s32)((u32)v - (((u32)image + at + 4) & ~3));
	u16 *bl = (u16 *)(image + at);
	bl[0] = 0xf000 | ((off >> 12) & 0x7ff);
	bl[1] = 0xe800 | ((off >> 1) & 0x7fe);
	return 0;
}

uint8_t *game_image(void)
{
	return image_base;
}

/* heap pages left executable keep the kernel from tearing the process down, it hangs on "Closing software" */
void unload_game(void)
{
	if (!image_base)
		return;
	protect(image_base, image_size, MEMPERM_READ | MEMPERM_WRITE);
	free(image_base);
	image_base = NULL;
}

#define TRAMP_MAX 256
#define TRAMP_SIZE 20

static u8 *tramp_base;
static const char *tramp_names[TRAMP_MAX];
static volatile u32 last_tramp;

/*
 * [PORT] Trace=1 routes every import through a stub that drops its own pc into
 * last_tramp, so the watchdog can say which call the game made last:
 *	ldr ip, [pc, #4]	@ &last_tramp
 *	str pc, [ip]
 *	ldr pc, [pc, #0]	@ real function
 */
static void *trampoline(u8 *base, u32 sym, void *fn, const char *name)
{
	u32 *t = (u32 *)(base + sym * TRAMP_SIZE);

	tramp_base = base;
	tramp_names[sym] = name;
	t[0] = 0xe59fc004;
	t[1] = 0xe58cf000;
	t[2] = 0xe59ff000;
	t[3] = (u32)&last_tramp;
	t[4] = (u32)fn;
	return t;
}

const char *last_import(void)
{
	u32 v = last_tramp;
	if (!tramp_base || !v)
		return "-";
	u32 sym = (v - (u32)tramp_base) / TRAMP_SIZE;
	return sym < TRAMP_MAX && tramp_names[sym] ? tramp_names[sym] : "?";
}

/* [PORT] Patch=offset:hexbytes,... with offsets relative to the image start */
static void apply_patches(u8 *image, u32 size)
{
	const char *list = config_get("PORT", "Patch");

	while (list && *list) {
		char *end;
		u32 off = strtoul(list, &end, 16);
		if (*end != ':')
			break;
		const char *hex = end + 1;
		u32 n = 0;
		while (isxdigit((unsigned char)hex[0]) && isxdigit((unsigned char)hex[1]) && off + n < size) {
			char byte[3] = { hex[0], hex[1], 0 };
			image[off + n++] = strtoul(byte, NULL, 16);
			hex += 2;
		}
		port_log("patched %lu bytes at image+%lx\n", n, off);
		list = strchr(hex, ',');
		if (list)
			list++;
	}
}

int load_game(const char *path, void **entry)
{
	size_t len;
	u8 *file = read_file(path, &len);
	if (!file) {
		port_log("can't read %s\n", path);
		return -1;
	}

	if (memcmp(file, "XE3U", 4)) {
		u8 *raw;
		size_t raw_len;
		if (lzma_unpack_alone(file, len, &raw, &raw_len)) {
			port_log("%s: bad lzma stream\n", path);
			free(file);
			return -1;
		}
		free(file);
		file = raw;
		len = raw_len;
	}

	xe3u_header hdr;
	memcpy(&hdr, file, sizeof(hdr));
	if (memcmp(hdr.magic, "XE3U", 4) || hdr.code_off + hdr.code_size > len) {
		port_log("%s: not an XE3U image\n", path);
		free(file);
		return -1;
	}

	u32 tramp_off = (hdr.mem_size + 15) & ~15;
	u32 veneer_off = tramp_off + TRAMP_MAX * TRAMP_SIZE;
	u32 map_size = (veneer_off + VENEER_MAX * 8 + 0xfff) & ~0xfff;
	u8 *image = memalign(0x1000, map_size);
	if (!image) {
		port_log("out of memory mapping %lu bytes\n", map_size);
		free(file);
		return -1;
	}
	memcpy(image, file + hdr.code_off, hdr.code_size);
	memset(image + hdr.code_size, 0, map_size - hdr.code_size);

	u32 delta = (u32)image - (hdr.link_base ? hdr.link_base : LINK_BASE_DEFAULT);
	const char **names = NULL;
	int trace = port_option("Trace", 0);
	u32 nnames = 0, missing = 0;

	const u8 *p = file + hdr.fixup_off;
	const u8 *end = p + hdr.fixup_size;
	while (p + 8 <= end) {
		u32 type = rd32(p);
		u32 size = rd32(p + 4);
		const u8 *d = p + 8;

		if (size < 8)
			break;

		if (type == FIX_IMPORTS) {
			nnames = rd16(d);
			names = calloc(nnames, sizeof(*names));
			d += 2;
			for (u32 i = 0; i < nnames; i++) {
				names[i] = (const char *)d;
				d += strlen((const char *)d) + 1;
			}
		} else if (type == FIX_RELOCS) {
			u32 n = rd32(d);
			for (u32 i = 0; i < n; i++) {
				u32 *w = (u32 *)(image + rd32(d + 4 + i * 4));
				*w += delta;
			}
		} else if (type == FIX_SLOTS) {
			u32 n = rd32(d);
			for (u32 i = 0; i < n; i++) {
				const u8 *e = d + 4 + i * 6;
				u32 off = rd16(e) << 16 | rd16(e + 2);
				u32 sym = rd16(e + 4);
				void *fn = NULL;

				if (sym < nnames)
					fn = import_lookup(names[sym]);
				if (!fn) {
					port_log("unresolved: %s\n", sym < nnames ? names[sym] : "?");
					fn = unimplemented;
					missing++;
				}
				if (trace && sym < TRAMP_MAX)
					fn = trampoline(image + tramp_off, sym, fn, names[sym]);
				*(u32 *)(image + off) = (u32)fn;
			}
		}
		p += size;
	}

	apply_patches(image, hdr.code_size);

	veneer_base = image + veneer_off;
	if (port_option("HideTouchHud", 1) || gl_bottom_ui())
		hud_hooks(image);

	port_log("image at %p, %lu bytes, %lu imports, %lu stubbed\n",
		image, hdr.mem_size, nnames, missing);
	free(names);

	/* trampolines keep pointing at the import names, so the file stays around when tracing */
	if (!trace)
		free(file);

	Result rc = protect(image, map_size, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
	if (R_FAILED(rc)) {
		port_log("can't make image executable: %08lx\n", rc);
		return -1;
	}

	/* the heap pages we copied code into may still be in the I-cache from whatever ran there before */
	if (port_option("FlushICache", 1)) {
		port_log("flushing i-cache\n");
		svcBackdoor(flush_icache);
	}

	image_base = image;
	image_size = map_size;
	*entry = image + hdr.entry;
	return 0;
}
