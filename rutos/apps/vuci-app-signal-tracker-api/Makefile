# VuCI API for signal-tracker (Puffin SDK), like the SDK's vuci-app-example-api:
# /api/signal_tracker/* (status and actions, through the daemon's ubus object) and
# /api/signal_tracker_config/* (/etc/config/signal_tracker).
DESTDIR ?= /
FILES_DIR := files

all: build

build:
	@echo "Nothing to build."

# Fixed modes whatever the checkout's umask: ubusd ignores ACL files that are group-writable.
install: build
	umask 022 && cd $(FILES_DIR) && for f in $$(find . -type f); do \
		install -D -m 644 $$f $(DESTDIR)/$$f || exit 1; \
	done

clean:
	@echo "Nothing to clean."

.PHONY: all build install clean
