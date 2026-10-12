#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>
#include <unistd.h>
#include <stdlib.h>

#include "port.h"
#include "gl.h"
#include "s3e.h"

/* the engine recurses a lot, libctru's default 32K is nowhere near enough */
u32 __stacksize__ = 1024 * 1024;

/* libctru hands half of app memory to the linear heap by default, the game heaps need it more */
u32 __ctru_linear_heap_size = 18 * 1024 * 1024;

/* test builds only: squeeze the heap down to what an Old 3DS app would get */
#ifdef HEAP_LIMIT_MB
u32 __ctru_heap_size = HEAP_LIMIT_MB * 1024 * 1024;
#else
extern u32 __ctru_heap_size;
#endif

static jmp_buf exit_jmp;
static volatile int running = 1;
static FILE *log_file;

void port_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);

	if (log_file) {
		va_start(ap, fmt);
		vfprintf(log_file, fmt, ap);
		va_end(ap);
		fflush(log_file);
	}
}

void port_exit(int code)
{
	longjmp(exit_jmp, code | 0x100);
}

/* runs beside the game and says where it is, even if the main thread is wedged */
static void watchdog(void *arg)
{
	while (running) {
		svcSleepThread(2000000000LL);
		if (running)
			port_log("[wd] frame %lu, last call %s, gpu %s, vblanks %lu\n",
				port_frame(), last_import(), gl_stage, gl_vblanks());
	}
}

static void wait_for_start(void)
{
	port_log("press START to exit\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gspWaitForVBlank();
	}
}

int main(void)
{
	void *entry;
	int rc;

	gfxInitDefault();
	chdir(DATA_DIR);
	log_file = fopen(DATA_DIR "/log.txt", "w");
	config_load();
	/* the bottom screen belongs to the touch UI unless it's turned off */
	if (!port_option("BottomScreen", 1))
		consoleInit(GFX_BOTTOM, NULL);
	port_log("app region %lu KB, heap %lu KB, linear heap %lu KB\n",
		osGetMemRegionSize(MEMREGION_APPLICATION) >> 10, __ctru_heap_size >> 10, __ctru_linear_heap_size >> 10);

	if (port_option("Watchdog", 0) && !threadCreate(watchdog, NULL, 16 * 1024, 0x18, -2, true))
		port_log("watchdog thread failed\n");
	gl_init();
	sound_init();

	if (load_game(DATA_DIR "/biohazard.s3e", &entry)) {
		wait_for_start();
		goto out;
	}

	port_log("starting game\n");

	rc = setjmp(exit_jmp);
	if (!rc)
		rc = ((int (*)(void))entry)();
	else
		rc &= 0xff;
	running = 0;
	port_log("game exited with %d\n", rc);

out:
	sound_exit();
	port_log("exit: sound down\n");
	unload_game();
	port_log("exit: image freed\n");
	C3D_Fini();
	port_log("exit: gpu down\n");
	if (log_file)
		fclose(log_file);
	gfxExit();
	return 0;
}
