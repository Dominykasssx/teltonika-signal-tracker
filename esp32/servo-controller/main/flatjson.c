#include "flatjson.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return p;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Parses a JSON string starting at the opening quote. Non-ASCII \u escapes become '?'. */
static const char *parse_string(const char *p, char *out, size_t cap)
{
	size_t n = 0;

	if (*p != '"')
		return NULL;
	p++;

	while (*p != '"') {
		char c = *p;

		if (c == '\0' || (unsigned char)c < 0x20)
			return NULL;
		p++;

		if (c == '\\') {
			char e = *p;

			if (e == '\0')
				return NULL;
			p++;
			switch (e) {
			case '"': c = '"'; break;
			case '\\': c = '\\'; break;
			case '/': c = '/'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'u': {
				unsigned v = 0;

				for (int i = 0; i < 4; i++) {
					int h = hex_value(*p);

					if (h < 0)
						return NULL;
					v = v * 16 + (unsigned)h;
					p++;
				}
				c = (v >= 0x20 && v < 0x80) ? (char)v : '?';
				break;
			}
			default:
				return NULL;
			}
		}

		if (n + 1 >= cap)
			return NULL;
		out[n++] = c;
	}

	out[n] = '\0';
	return p + 1;
}

/* Strict JSON number grammar; strtod alone would also accept hex, inf and nan. */
static const char *parse_number(const char *p, double *out)
{
	const char *start = p;

	if (*p == '-')
		p++;
	if (*p == '0') {
		p++;
	} else if (*p >= '1' && *p <= '9') {
		while (*p >= '0' && *p <= '9')
			p++;
	} else {
		return NULL;
	}
	if (*p == '.') {
		p++;
		if (!(*p >= '0' && *p <= '9'))
			return NULL;
		while (*p >= '0' && *p <= '9')
			p++;
	}
	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '+' || *p == '-')
			p++;
		if (!(*p >= '0' && *p <= '9'))
			return NULL;
		while (*p >= '0' && *p <= '9')
			p++;
	}

	char buf[40];
	size_t len = (size_t)(p - start);

	if (len >= sizeof(buf))
		return NULL;
	memcpy(buf, start, len);
	buf[len] = '\0';
	*out = strtod(buf, NULL);
	if (!isfinite(*out))
		return NULL;
	return p;
}

static const char *parse_literal(const char *p, const char *word)
{
	size_t len = strlen(word);

	return strncmp(p, word, len) == 0 ? p + len : NULL;
}

int fj_parse(const char *text, struct fj_object *out)
{
	const char *p = skip_ws(text);

	out->count = 0;
	if (*p != '{')
		return FJ_ERR_SYNTAX;
	p = skip_ws(p + 1);

	if (*p == '}')
		goto end;

	for (;;) {
		struct fj_field *f;

		if (out->count >= FJ_MAX_FIELDS)
			return FJ_ERR_TOO_BIG;
		f = &out->fields[out->count];

		p = parse_string(p, f->key, sizeof(f->key));
		if (!p)
			return FJ_ERR_SYNTAX;
		p = skip_ws(p);
		if (*p != ':')
			return FJ_ERR_SYNTAX;
		p = skip_ws(p + 1);

		f->str[0] = '\0';
		f->num = 0;
		f->boolean = false;

		if (*p == '"') {
			f->type = FJ_STRING;
			p = parse_string(p, f->str, sizeof(f->str));
		} else if (*p == '{' || *p == '[') {
			return FJ_ERR_NESTED;
		} else if (*p == 't') {
			f->type = FJ_BOOL;
			f->boolean = true;
			p = parse_literal(p, "true");
		} else if (*p == 'f') {
			f->type = FJ_BOOL;
			p = parse_literal(p, "false");
		} else if (*p == 'n') {
			f->type = FJ_NULL;
			p = parse_literal(p, "null");
		} else {
			f->type = FJ_NUMBER;
			p = parse_number(p, &f->num);
		}
		if (!p)
			return FJ_ERR_SYNTAX;
		out->count++;

		p = skip_ws(p);
		if (*p == ',') {
			p = skip_ws(p + 1);
			continue;
		}
		if (*p == '}')
			break;
		return FJ_ERR_SYNTAX;
	}

end:
	p = skip_ws(p + 1);
	return *p == '\0' ? FJ_OK : FJ_ERR_SYNTAX;
}

const struct fj_field *fj_get(const struct fj_object *obj, const char *key)
{
	for (int i = 0; i < obj->count; i++) {
		if (strcmp(obj->fields[i].key, key) == 0)
			return &obj->fields[i];
	}
	return NULL;
}

bool fj_get_string(const struct fj_object *obj, const char *key, const char **out)
{
	const struct fj_field *f = fj_get(obj, key);

	if (!f || f->type != FJ_STRING)
		return false;
	*out = f->str;
	return true;
}

bool fj_get_number(const struct fj_object *obj, const char *key, double *out)
{
	const struct fj_field *f = fj_get(obj, key);

	if (!f || f->type != FJ_NUMBER)
		return false;
	*out = f->num;
	return true;
}

bool fj_get_bool(const struct fj_object *obj, const char *key, bool *out)
{
	const struct fj_field *f = fj_get(obj, key);

	if (!f || f->type != FJ_BOOL)
		return false;
	*out = f->boolean;
	return true;
}

bool fj_get_int(const struct fj_object *obj, const char *key, int *out)
{
	double v;

	if (!fj_get_number(obj, key, &v))
		return false;
	if (v != floor(v) || v < INT_MIN || v > INT_MAX)
		return false;
	*out = (int)v;
	return true;
}

bool fj_is_null(const struct fj_object *obj, const char *key)
{
	const struct fj_field *f = fj_get(obj, key);

	return f && f->type == FJ_NULL;
}
