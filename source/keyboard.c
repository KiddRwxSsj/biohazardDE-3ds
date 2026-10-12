#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "s3e.h"

/* the touch build only polls a few debug keys, real buttons go through controls.c */

static uint32_t key_by_name(const char *s, int len)
{
	static const struct { const char *name; uint32_t key; } names[] = {
		{ "A", KEY_A }, { "B", KEY_B }, { "X", KEY_X }, { "Y", KEY_Y },
		{ "L", KEY_L }, { "R", KEY_R }, { "START", KEY_START }, { "SELECT", KEY_SELECT },
		{ "UP", KEY_DUP }, { "DOWN", KEY_DDOWN }, { "LEFT", KEY_DLEFT }, { "RIGHT", KEY_DRIGHT },
	};

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if ((int)strlen(names[i].name) == len && !strncasecmp(names[i].name, s, len))
			return names[i].key;
	return 0;
}

/* [PORT] AutoKeys=ms:KEY:duration,... for unattended test runs, ms counted from the first poll */
uint32_t auto_keys(void)
{
	static const char *list;
	static int checked;
	static uint64_t start;
	uint64_t now = s3eTimerGetMs();
	uint32_t keys = 0;

	if (!checked) {
		list = config_get("PORT", "AutoKeys");
		checked = 1;
	}
	if (!list)
		return 0;
	if (!start)
		start = now;
	now -= start;

	for (const char *p = list; *p;) {
		char *end;
		uint32_t at = strtoul(p, &end, 10);
		const char *name = end + 1;
		const char *colon = strchr(name, ':');
		if (!colon)
			break;
		uint32_t len = strtoul(colon + 1, &end, 10);
		if (now >= at && now < at + len)
			keys |= key_by_name(name, colon - name);
		p = *end ? end + 1 : end;
	}
	return keys;
}

int32_t s3eKeyboardRegister(int32_t ev, s3e_callback cb, void *user)
{
	return S3E_RESULT_SUCCESS;
}

int32_t s3eKeyboardUnRegister(int32_t ev, s3e_callback cb)
{
	return S3E_RESULT_SUCCESS;
}

int32_t s3eKeyboardUpdate(void)
{
	return S3E_RESULT_SUCCESS;
}

int32_t s3eKeyboardGetState(int32_t key)
{
	return 0;
}

int32_t s3eKeyboardAnyKey(void)
{
	return 0;
}

int32_t s3eKeyboardClearState(void)
{
	return S3E_RESULT_SUCCESS;
}

int32_t s3eKeyboardSetInt(int32_t prop, int32_t value)
{
	return S3E_RESULT_SUCCESS;
}

const char *s3eKeyboardGetDisplayName(int32_t key)
{
	return "";
}
