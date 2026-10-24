/*
 * Minimal parser for flat, single-line JSON objects.
 *
 * Accepts objects whose values are strings, numbers, booleans or null.
 * Nested objects and arrays are rejected; the USB serial protocol never needs them.
 * The same file is used by the ESP32 firmware and the RUTOS daemon.
 */
#ifndef FLATJSON_H
#define FLATJSON_H

#include <stdbool.h>
#include <stddef.h>

#define FJ_MAX_FIELDS 16
#define FJ_KEY_MAX 24
#define FJ_STR_MAX 96

enum fj_type {
	FJ_STRING,
	FJ_NUMBER,
	FJ_BOOL,
	FJ_NULL,
};

struct fj_field {
	char key[FJ_KEY_MAX];
	enum fj_type type;
	char str[FJ_STR_MAX];
	double num;
	bool boolean;
};

struct fj_object {
	int count;
	struct fj_field fields[FJ_MAX_FIELDS];
};

enum fj_error {
	FJ_OK = 0,
	FJ_ERR_SYNTAX = -1,
	FJ_ERR_NESTED = -2,
	FJ_ERR_TOO_BIG = -3,
};

int fj_parse(const char *text, struct fj_object *out);

const struct fj_field *fj_get(const struct fj_object *obj, const char *key);
bool fj_get_string(const struct fj_object *obj, const char *key, const char **out);
bool fj_get_number(const struct fj_object *obj, const char *key, double *out);
bool fj_get_bool(const struct fj_object *obj, const char *key, bool *out);
/* True only for numbers with no fractional part that fit in an int. */
bool fj_get_int(const struct fj_object *obj, const char *key, int *out);
bool fj_is_null(const struct fj_object *obj, const char *key);

#endif
