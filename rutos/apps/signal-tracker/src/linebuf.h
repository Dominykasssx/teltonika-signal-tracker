/*
 * Splits a serial byte stream into lines. Handles partial reads, several lines per
 * read, CRLF endings and over-long lines (dropped up to the next newline).
 */
#ifndef LINEBUF_H
#define LINEBUF_H

#include <stdbool.h>
#include <stddef.h>

#define LINEBUF_MAX 320

/* line is NUL-terminated without the line ending; NULL means an over-long line was dropped. */
typedef void (*linebuf_cb)(void *ctx, const char *line);

struct linebuf {
	char buf[LINEBUF_MAX];
	size_t len;
	bool overflow;
};

void linebuf_reset(struct linebuf *lb);
void linebuf_feed(struct linebuf *lb, const char *data, size_t n, linebuf_cb cb, void *ctx);

#endif
