#include <stdlib.h>
#include <string.h>
#include <lzma.h>

#include "s3e.h"

typedef struct {
	void *buf;
	uint32_t len;
} read_req;

typedef uint32_t (*read_cb)(read_req *req, void *user);

typedef struct stream {
	struct stream *next;
	lzma_stream z;
	read_cb cb;
	void *user;
	int done;
	uint8_t in[0x4000];
} stream;

/* the game re-inits a stream for almost every small read, reusing the lzma_stream keeps its dictionary */
static stream *pool;

void *s3eCompressionDecompInit(int32_t algo, void *cb, void *user)
{
	stream *s = pool;

	if (algo != 0) {
		port_log("decomp algo %ld not supported\n", algo);
		return NULL;
	}

	if (s) {
		pool = s->next;
	} else {
		s = calloc(1, sizeof(*s));
		if (!s)
			return NULL;
		s->z = (lzma_stream)LZMA_STREAM_INIT;
	}

	if (lzma_alone_decoder(&s->z, UINT64_MAX) != LZMA_OK) {
		lzma_end(&s->z);
		free(s);
		return NULL;
	}
	s->z.next_in = NULL;
	s->z.avail_in = 0;
	s->cb = cb;
	s->user = user;
	s->done = 0;
	return s;
}

int32_t s3eCompressionDecompRead(void *h, void *buf, uint32_t *len)
{
	stream *s = h;

	s->z.next_out = buf;
	s->z.avail_out = *len;

	while (s->z.avail_out && !s->done) {
		if (!s->z.avail_in) {
			read_req req = { s->in, sizeof(s->in) };
			uint32_t n = s->cb(&req, s->user);
			if (!n || n > sizeof(s->in))
				break;
			s->z.next_in = s->in;
			s->z.avail_in = n;
		}

		lzma_ret r = lzma_code(&s->z, LZMA_RUN);
		if (r == LZMA_STREAM_END) {
			s->done = 1;
		} else if (r != LZMA_OK) {
			*len -= s->z.avail_out;
			return S3E_RESULT_ERROR;
		}
	}

	*len -= s->z.avail_out;
	return S3E_RESULT_SUCCESS;
}

int32_t s3eCompressionDecompFinal(void *h)
{
	stream *s = h;

	if (s) {
		s->next = pool;
		pool = s;
	}
	return S3E_RESULT_SUCCESS;
}

int lzma_unpack_alone(const uint8_t *src, size_t len, uint8_t **out, size_t *out_len)
{
	lzma_stream z = LZMA_STREAM_INIT;
	uint64_t size = 0;

	if (len < 13)
		return -1;
	for (int i = 12; i >= 5; i--)
		size = size << 8 | src[i];
	if (size > 64 * 1024 * 1024)
		return -1;

	*out = malloc(size);
	if (!*out || lzma_alone_decoder(&z, UINT64_MAX) != LZMA_OK) {
		free(*out);
		return -1;
	}

	z.next_in = src;
	z.avail_in = len;
	z.next_out = *out;
	z.avail_out = size;
	lzma_ret r = lzma_code(&z, LZMA_FINISH);
	lzma_end(&z);

	if (r != LZMA_STREAM_END || z.avail_out) {
		free(*out);
		return -1;
	}
	*out_len = size;
	return 0;
}
