# VuCI WebUI page for signal-tracker (Services -> Signal Tracker), built like the SDK's
# vuci-app-example-ui: vite compiles src/views into dest/.
NPM ?= npm
DESTDIR ?= /
# Not the example's shared ../../tmp/vuci, so the two apps do not reuse each other's install.
DL_DIR ?= ../../tmp/vuci-signal-tracker
FILES_DIR := files
COMPILED_DIR := dest
NODE_MODULES_STAMP := $(DL_DIR)/.node_modules_installed

all: build

# Without the example's unused vue-devtools and dynamic-import plugins and with the versions
# pinned in package-lock.json: with those, the SDK's npm 10.8 fails ("reading 'edgesOut'").
$(NODE_MODULES_STAMP): package.json package-lock.json
	rm -rf "$(DL_DIR)/node_modules"
	mkdir -p "$(DL_DIR)"
	cp -f package*.json "$(DL_DIR)"
	$(NPM) install --prefix "$(DL_DIR)"
	touch "$@"

build: $(NODE_MODULES_STAMP)
	rm -rf node_modules
	ln -s "$(DL_DIR)/node_modules" node_modules
	@$(NPM) run compile

# Fixed modes whatever the checkout's umask.
install: build
	umask 022 && cd $(FILES_DIR) && for f in $$(find . -type f); do \
		install -D -m 644 $$f $(DESTDIR)/$$f || exit 1; \
	done
	umask 022 && install -d $(DESTDIR)/www/views/services
	install -m 644 $(COMPILED_DIR)/services/* $(DESTDIR)/www/views/services/

clean:
	rm -rf $(COMPILED_DIR) node_modules

.PHONY: all build install clean
