#ifndef S3E_H
#define S3E_H

#include <stdint.h>
#include "port.h"

#define S3E_RESULT_SUCCESS 0
#define S3E_RESULT_ERROR 1

typedef struct s3eFile s3eFile;

void *s3eMallocBase(uint32_t size);
void *s3eReallocBase(void *p, uint32_t size);
void s3eFreeBase(void *p);
int32_t s3eMemoryGetInt(int32_t prop);
int32_t s3eMemorySetInt(int32_t prop, int32_t value);
int32_t s3eMemoryHeapCreate(int32_t a, int32_t b, int32_t c, int32_t d);
int32_t s3eMemorySetUserMemMgr(void *mgr);

int32_t s3eConfigGetInt(const char *group, const char *key, int32_t *out);
int32_t s3eConfigGetString(const char *group, const char *key, char *out);

s3eFile *s3eFileOpen(const char *name, const char *mode);
s3eFile *s3eFileOpenFromMemory(void *buf, uint32_t size);
int32_t s3eFileClose(s3eFile *f);
uint32_t s3eFileRead(void *buf, uint32_t size, uint32_t count, s3eFile *f);
uint32_t s3eFileWrite(const void *buf, uint32_t size, uint32_t count, s3eFile *f);
int32_t s3eFileSeek(s3eFile *f, int32_t off, int32_t origin);
int32_t s3eFileTell(s3eFile *f);
int32_t s3eFileGetSize(s3eFile *f);
int32_t s3eFileGetChar(s3eFile *f);
int32_t s3eFileFlush(s3eFile *f);
int32_t s3eFileCheckExists(const char *name);
int32_t s3eFileDelete(const char *name);
int32_t s3eFileMakeDirectory(const char *name);
int32_t s3eFileGetError(void);
int32_t s3eFileAddUserFileSys(const void *fs);

void *s3eCompressionDecompInit(int32_t algo, void *cb, void *user);
int32_t s3eCompressionDecompRead(void *h, void *buf, uint32_t *len);
int32_t s3eCompressionDecompFinal(void *h);

int32_t s3eDeviceGetInt(int32_t prop);
const char *s3eDeviceGetString(int32_t prop);
int32_t s3eDeviceCheckQuitRequest(void);
int32_t s3eDeviceCheckPauseRequest(void);
int32_t s3eDeviceYield(int32_t ms);
int32_t s3eDeviceYieldUntilEvent(int32_t ms);
void s3eDeviceExit(int32_t code);

uint64_t s3eTimerGetMs(void);
uint64_t s3eTimerGetUTC(void);
int32_t s3eTimerGetInt(int32_t prop);
int32_t s3eTimerGetLocaltimeOffset(void);

int32_t s3eDebugTraceLine(const char *s);
int32_t s3eDebugOutputString(const char *s);
int32_t s3eDebugPrint(int32_t x, int32_t y, const char *s, int32_t wrap);
int32_t s3eDebugErrorShow(int32_t type, const char *s);
int32_t s3eDebugAssertShow(int32_t type, const char *s);

int32_t s3eSurfaceGetInt(int32_t prop);
int32_t s3eGLGetInt(int32_t prop);
void *s3eGLGetNativeWindow(void);

int32_t s3ePointerRegister(int32_t ev, s3e_callback cb, void *user);
int32_t s3ePointerUnRegister(int32_t ev, s3e_callback cb);
int32_t s3ePointerUpdate(void);
int32_t s3ePointerGetState(int32_t button);
int32_t s3ePointerGetX(void);
int32_t s3ePointerGetY(void);
int32_t s3ePointerGetInt(int32_t prop);

int32_t s3eKeyboardRegister(int32_t ev, s3e_callback cb, void *user);
int32_t s3eKeyboardUnRegister(int32_t ev, s3e_callback cb);
int32_t s3eKeyboardUpdate(void);
int32_t s3eKeyboardGetState(int32_t key);
int32_t s3eKeyboardAnyKey(void);
int32_t s3eKeyboardClearState(void);
int32_t s3eKeyboardSetInt(int32_t prop, int32_t value);
const char *s3eKeyboardGetDisplayName(int32_t key);

void sound_init(void);
void sound_exit(void);
void sound_stats(uint32_t *loud, uint32_t *gens);
int32_t s3eSoundGetInt(int32_t prop);
int32_t s3eSoundSetInt(int32_t prop, int32_t value);
int32_t s3eSoundGetFreeChannel(void);
int32_t s3eSoundChannelRegister(int32_t ch, int32_t ev, s3e_callback cb, void *user);
int32_t s3eSoundChannelUnRegister(int32_t ch, int32_t ev);
int32_t s3eSoundChannelPlay(int32_t ch, int16_t *start, uint32_t num, int32_t repeat, int32_t loop_from);
int32_t s3eSoundChannelStop(int32_t ch);
int32_t s3eSoundChannelPause(int32_t ch);
int32_t s3eSoundChannelResume(int32_t ch);
int32_t s3eSoundChannelGetInt(int32_t ch, int32_t prop);
int32_t s3eSoundChannelSetInt(int32_t ch, int32_t prop, int32_t value);
int32_t s3eExtGet(const char *name, void *buf, int32_t size);
int32_t s3eSecureStorageGet(void *buf, uint16_t size);
int32_t s3eSecureStoragePut(const void *buf, uint16_t size);

#endif
