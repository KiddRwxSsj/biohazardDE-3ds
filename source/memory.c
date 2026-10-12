#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "s3e.h"

#define NHEAPS 8

/* 8 bytes so the game still gets the 8-byte alignment malloc promised */
typedef struct {
	uint32_t heap;
	uint32_t size;
} block;

static int cur_heap;
static uint32_t used[NHEAPS];
static uint32_t limits[NHEAPS];

static uint32_t heap_limit(int heap)
{
	char key[16];
	int32_t v;

	if (limits[heap])
		return limits[heap];
	if (heap)
		snprintf(key, sizeof(key), "MemSize%d", heap);
	else
		snprintf(key, sizeof(key), "MemSize");
	if (!s3eConfigGetInt("S3E", key, &v) && v > 0)
		limits[heap] = v;
	else
		limits[heap] = heap ? 16000000 : 17000000;
	return limits[heap];
}

static void *alloc_on(int heap, uint32_t size)
{
	if (used[heap] + size > heap_limit(heap))
		return NULL;
	/* the game leans on fresh allocations being zeroed, phone allocators happened to give it that */
	block *b = calloc(1, sizeof(block) + size);
	if (!b)
		return NULL;
	b->heap = heap;
	b->size = size;
	used[heap] += size;
	return b + 1;
}

void *s3eMallocBase(uint32_t size)
{
	return alloc_on(cur_heap, size);
}

void s3eFreeBase(void *p)
{
	if (!p)
		return;
	block *b = (block *)p - 1;
	used[b->heap] -= b->size;
	free(b);
}

void *s3eReallocBase(void *p, uint32_t size)
{
	if (!p)
		return alloc_on(cur_heap, size);
	if (!size) {
		s3eFreeBase(p);
		return NULL;
	}

	block *b = (block *)p - 1;
	if (size > b->size && used[b->heap] + size - b->size > heap_limit(b->heap))
		return NULL;

	block *n = realloc(b, sizeof(block) + size);
	if (!n)
		return NULL;
	if (size > n->size)
		memset((uint8_t *)(n + 1) + n->size, 0, size - n->size);
	used[n->heap] += size - n->size;
	n->size = size;
	return n + 1;
}

int32_t s3eMemoryGetInt(int32_t prop)
{
	uint32_t limit = heap_limit(cur_heap);
	uint32_t avail = limit > used[cur_heap] ? limit - used[cur_heap] : 0;

	switch (prop) {
	case 0: return cur_heap;
	case 1: return used[cur_heap];
	case 2: return limit;
	case 3:
	case 4: return avail;
	}
	return 0;
}

int32_t s3eMemorySetInt(int32_t prop, int32_t value)
{
	if (prop == 0 && value >= 0 && value < NHEAPS)
		cur_heap = value;
	return S3E_RESULT_SUCCESS;
}

int32_t s3eMemoryHeapCreate(int32_t a, int32_t b, int32_t c, int32_t d)
{
	port_log("s3eMemoryHeapCreate(%lx, %lx, %lx, %lx)\n", a, b, c, d);
	return S3E_RESULT_SUCCESS;
}

int32_t s3eMemorySetUserMemMgr(void *mgr)
{
	return S3E_RESULT_SUCCESS;
}
