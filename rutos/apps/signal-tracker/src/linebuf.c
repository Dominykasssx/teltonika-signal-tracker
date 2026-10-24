#include "linebuf.h"

void linebuf_reset(struct linebuf *lb)
{
	lb->len = 0;
	lb->overflow = false;
}

void linebuf_feed(struct linebuf *lb, const char *data, size_t n, linebuf_cb cb, void *ctx)
{
	for (size_t i = 0; i < n; i++) {
		char c = data[i];

		if (c == '\r' || c == '\0')
			continue;

		if (c != '\n') {
			if (lb->len + 1 < sizeof(lb->buf))
				lb->buf[lb->len++] = c;
			else
				lb->overflow = true;
			continue;
		}

		if (lb->overflow) {
			cb(ctx, NULL);
		} else if (lb->len > 0) {
			lb->buf[lb->len] = '\0';
			cb(ctx, lb->buf);
		}
		linebuf_reset(lb);
	}
}
