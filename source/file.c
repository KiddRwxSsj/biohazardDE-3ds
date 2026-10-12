#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "s3e.h"

#define MAX_USERFS 8

/* layout the game hands to s3eFileAddUserFileSys, all plain AAPCS */
typedef struct {
	void *(*open)(const char *name, const char *mode);
	uint32_t (*read)(void *buf, uint32_t size, uint32_t count, void *h);
	int32_t (*eof)(void *h);
	int32_t (*seek)(void *h, int32_t off, int32_t origin);
	int32_t (*tell)(void *h);
	int32_t (*close)(void *h);
	void *list_open;
	void *list_next;
	void *list_close;
} userfs;

struct s3eFile {
	FILE *fp;
	const userfs *fs;
	void *h;
};

static userfs fs_list[MAX_USERFS];
static int verbose_files = -1;
static int nfs;

static char *host_path(const char *name, char *out, size_t len)
{
	static const char *const prefixes[] = { "rom://", "ram://", "raw://", "tmp://" };

	for (int i = 0; i < 4; i++) {
		size_t n = strlen(prefixes[i]);
		if (!strncmp(name, prefixes[i], n)) {
			name += n;
			break;
		}
	}
	while (*name == '/' || *name == '\\')
		name++;

	snprintf(out, len, DATA_DIR "/%s", name);
	for (char *c = out; *c; c++)
		if (*c == '\\')
			*c = '/';
	return out;
}

static s3eFile *wrap(FILE *fp, const userfs *fs, void *h)
{
	s3eFile *f = malloc(sizeof(*f));
	f->fp = fp;
	f->fs = fs;
	f->h = h;
	return f;
}

static s3eFile *open_user(const char *name, const char *mode)
{
	for (int i = nfs - 1; i >= 0; i--) {
		void *h = fs_list[i].open(name, mode);
		if (h)
			return wrap(NULL, &fs_list[i], h);
	}
	return NULL;
}

int32_t s3eFileAddUserFileSys(const void *fs)
{
	if (nfs == MAX_USERFS)
		return S3E_RESULT_ERROR;
	memcpy(&fs_list[nfs++], fs, sizeof(userfs));
	return S3E_RESULT_SUCCESS;
}

s3eFile *s3eFileOpen(const char *name, const char *mode)
{
	char path[256];

	if (!mode)
		mode = "rb";
	if (verbose_files < 0)
		verbose_files = port_option("LogFiles", 0);
	if (strchr(mode, 'r')) {
		s3eFile *f = open_user(name, mode);
		if (f) {
			if (verbose_files)
				port_log("open %s %s: archive\n", name, mode);
			return f;
		}
	}

	FILE *fp = fopen(host_path(name, path, sizeof(path)), mode);
	if (verbose_files)
		port_log("open %s %s: %s\n", name, mode, fp ? "ok" : "missing");
	if (!fp)
		return NULL;
	setvbuf(fp, NULL, _IOFBF, 32 * 1024);
	return wrap(fp, NULL, NULL);
}

s3eFile *s3eFileOpenFromMemory(void *buf, uint32_t size)
{
	FILE *fp = fmemopen(buf, size, "r+b");
	return fp ? wrap(fp, NULL, NULL) : NULL;
}

int32_t s3eFileClose(s3eFile *f)
{
	if (!f)
		return S3E_RESULT_ERROR;
	if (f->fs)
		f->fs->close(f->h);
	else
		fclose(f->fp);
	free(f);
	return S3E_RESULT_SUCCESS;
}

uint32_t s3eFileRead(void *buf, uint32_t size, uint32_t count, s3eFile *f)
{
	if (!f || !size)
		return 0;
	if (f->fs)
		return f->fs->read(buf, size, count, f->h);
	return fread(buf, size, count, f->fp);
}

uint32_t s3eFileWrite(const void *buf, uint32_t size, uint32_t count, s3eFile *f)
{
	if (!f || f->fs || !size)
		return 0;
	return fwrite(buf, size, count, f->fp);
}

int32_t s3eFileSeek(s3eFile *f, int32_t off, int32_t origin)
{
	if (!f)
		return S3E_RESULT_ERROR;
	if (f->fs)
		return f->fs->seek(f->h, off, origin);
	return fseek(f->fp, off, origin) ? S3E_RESULT_ERROR : S3E_RESULT_SUCCESS;
}

int32_t s3eFileTell(s3eFile *f)
{
	if (!f)
		return -1;
	if (f->fs)
		return f->fs->tell(f->h);
	return ftell(f->fp);
}

int32_t s3eFileGetSize(s3eFile *f)
{
	int32_t cur, size;

	if (!f)
		return -1;
	cur = s3eFileTell(f);
	s3eFileSeek(f, 0, SEEK_END);
	size = s3eFileTell(f);
	s3eFileSeek(f, cur, SEEK_SET);
	return size;
}

int32_t s3eFileGetChar(s3eFile *f)
{
	uint8_t c;

	if (!f)
		return -1;
	if (f->fs)
		return f->fs->read(&c, 1, 1, f->h) == 1 ? c : -1;
	return fgetc(f->fp);
}

int32_t s3eFileFlush(s3eFile *f)
{
	if (f && f->fp)
		fflush(f->fp);
	return S3E_RESULT_SUCCESS;
}

int32_t s3eFileCheckExists(const char *name)
{
	char path[256];
	struct stat st;

	s3eFile *f = open_user(name, "rb");
	if (f) {
		s3eFileClose(f);
		return 1;
	}
	return !stat(host_path(name, path, sizeof(path)), &st);
}

int32_t s3eFileDelete(const char *name)
{
	char path[256];
	return remove(host_path(name, path, sizeof(path))) ? S3E_RESULT_ERROR : S3E_RESULT_SUCCESS;
}

int32_t s3eFileMakeDirectory(const char *name)
{
	char path[256];
	mkdir(host_path(name, path, sizeof(path)), 0777);
	return S3E_RESULT_SUCCESS;
}

int32_t s3eFileGetError(void)
{
	return 0;
}

int32_t s3eSecureStorageGet(void *buf, uint16_t size)
{
	FILE *fp = fopen(DATA_DIR "/secure.bin", "rb");
	if (!fp)
		return S3E_RESULT_ERROR;
	fread(buf, 1, size, fp);
	fclose(fp);
	return S3E_RESULT_SUCCESS;
}

int32_t s3eSecureStoragePut(const void *buf, uint16_t size)
{
	FILE *fp = fopen(DATA_DIR "/secure.bin", "wb");
	if (!fp)
		return S3E_RESULT_ERROR;
	fwrite(buf, 1, size, fp);
	fclose(fp);
	return S3E_RESULT_SUCCESS;
}
