# signal-tracker service for the RUTOS Puffin SDK: the daemon and its init script (which
# writes /etc/config/signal_tracker with the default settings).
# `source init-build-env` provides CC/CFLAGS/LDFLAGS; `puf apps build` runs `make`,
# `make install` (DESTDIR=staging) and `make clean`.
CC ?= gcc
CFLAGS ?= -O2
DESTDIR ?= /
FILES_DIR := files

all: signal-tracker

signal-tracker: src/main.c
	$(CC) $(CFLAGS) -std=gnu99 -D_GNU_SOURCE -Wall -o $@ $< $(LDFLAGS) -lubus -lubox -luci -lm

# Fixed modes whatever the checkout's umask.
install: all
	umask 022 && install -d $(DESTDIR)/usr/sbin
	install -m 755 signal-tracker $(DESTDIR)/usr/sbin/
	umask 022 && cd $(FILES_DIR) && for f in $$(find . -type f); do \
		install -D -m 644 $$f $(DESTDIR)/$$f || exit 1; \
	done
	chmod 755 $(DESTDIR)/etc/init.d/signal-tracker

clean:
	rm -f signal-tracker

.PHONY: all install clean
