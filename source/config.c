#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "port.h"
#include "s3e.h"

typedef struct {
	char *group;
	char *key;
	char *value;
} entry;

static entry *entries;
static int nentries, cap;

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static char *upper(char *s)
{
	for (char *c = s; *c; c++)
		*c = toupper((unsigned char)*c);
	return s;
}

static char *unquote(char *s)
{
	size_t n = strlen(s);
	if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
		s[n - 1] = 0;
		s++;
	}
	return s;
}

static void strip_comment(char *s)
{
	if (s[0] == '#' || (s[0] == '/' && s[1] == '/')) {
		*s = 0;
		return;
	}
	for (char *c = s + 1; *c; c++) {
		if (!isspace((unsigned char)c[-1]))
			continue;
		if (*c == '#' || (c[0] == '/' && c[1] == '/')) {
			*c = 0;
			return;
		}
	}
}

static void set(const char *group, const char *key, const char *value)
{
	if (nentries == cap) {
		cap = cap ? cap * 2 : 128;
		entries = realloc(entries, cap * sizeof(*entries));
	}
	entries[nentries++] = (entry){ strdup(group), strdup(key), strdup(value) };
}

const char *config_get(const char *group, const char *key)
{
	for (int i = nentries - 1; i >= 0; i--)
		if (!strcasecmp(entries[i].group, group) && !strcasecmp(entries[i].key, key))
			return entries[i].value;
	return NULL;
}

int port_option(const char *key, int def)
{
	const char *v = config_get("PORT", key);
	return v ? atoi(v) : def;
}

/* {OS=...} blocks are evaluated as Android so we get the same settings the phone build ran with */
static int eval_condition(char *c)
{
	c = trim(c);
	if (!*c)
		return 1;

	if (!strncasecmp(c, "OS=", 3)) {
		for (char *tok = strtok(c + 3, ","); tok; tok = strtok(NULL, ","))
			if (!strcasecmp(trim(tok), "ANDROID"))
				return 1;
		return 0;
	}

	if (*c == '[') {
		char *ge = strchr(c, ']');
		char *eq = strchr(c, '=');
		if (!ge || !eq)
			return 0;
		*ge = 0;
		*eq = 0;
		char *val = eq + 1;
		if (*val == '=')
			val++;
		const char *cur = config_get(trim(c + 1), trim(ge + 1));
		return cur && !strcmp(cur, unquote(trim(val)));
	}

	return 0;
}

static void load_icf(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return;

	char line[4096], group[64] = "";
	int active = 1;

	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);

		if (*s == '{') {
			char *e = strchr(s, '}');
			if (e) {
				*e = 0;
				active = eval_condition(s + 1);
			}
			continue;
		}

		strip_comment(s);
		s = trim(s);
		if (!*s)
			continue;

		if (*s == '[') {
			char *e = strchr(s, ']');
			if (e) {
				*e = 0;
				snprintf(group, sizeof(group), "%s", upper(s + 1));
			}
			continue;
		}

		char *eq = strchr(s, '=');
		if (active && eq && *group) {
			*eq = 0;
			set(group, upper(trim(s)), unquote(trim(eq + 1)));
		}
	}
	fclose(f);
}

void config_load(void)
{
	load_icf(DATA_DIR "/s3e.icf");
	load_icf(DATA_DIR "/app.icf");
	load_icf(DATA_DIR "/port.icf");
}

int32_t s3eConfigGetInt(const char *group, const char *key, int32_t *out)
{
	const char *v = config_get(group, key);
	char *end;

	if (!v)
		return S3E_RESULT_ERROR;
	long n = strtol(v, &end, 0);
	if (end == v)
		return S3E_RESULT_ERROR;
	*out = n;
	return S3E_RESULT_SUCCESS;
}

int32_t s3eConfigGetString(const char *group, const char *key, char *out)
{
	const char *v = config_get(group, key);

	if (!v)
		return S3E_RESULT_ERROR;
	strcpy(out, v);
	return S3E_RESULT_SUCCESS;
}
