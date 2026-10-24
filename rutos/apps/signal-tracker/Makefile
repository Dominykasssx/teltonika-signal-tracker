# signal-tracker daemon for the RUTOS Puffin SDK.
# `source init-build-env` provides CC/CFLAGS/LDFLAGS for the target; `puf apps build`
# runs `make`, `make install` (DESTDIR=staging) and `make clean`.
CC ?= gcc
CFLAGS ?= -Wall -O2
DESTDIR ?= /
PREFIX ?= /usr
FILES_DIR := files

BINARY = signal-tracker
SRCS = $(addprefix src/,main.c tracker.c esp_link.c motion.c scan.c score.c signal.c \
	linebuf.c flatjson.c ev.c config.c config_uci.c serial.c gsm.c sim.c)
LIBS = -lubus -lubox -luci -lm

all: $(BINARY)

$(BINARY): $(SRCS) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -std=gnu99 -D_GNU_SOURCE -Wall -o $@ $(SRCS) $(LDFLAGS) $(LIBS)

install: $(BINARY)
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 755 $(BINARY) $(DESTDIR)$(PREFIX)/sbin/
	cp -af $(FILES_DIR)/* $(DESTDIR)/
	chmod 755 $(DESTDIR)/etc/init.d/signal-tracker

# Host unit tests (native compiler, no SDK needed): make test CC=cc
test:
	$(MAKE) -C tests test

clean:
	rm -f $(BINARY)
	$(MAKE) -C tests clean

.PHONY: all install test clean
