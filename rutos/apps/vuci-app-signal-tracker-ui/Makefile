NPM ?= npm

DESTDIR ?= /
DL_DIR ?= ../../tmp/vuci
FILES_DIR := files
COMPILED_DIR := dest
NODE_MODULES_STAMP := $(DL_DIR)/.node_modules_installed

define setup_node_modules
	rm -rf "$(DL_DIR)/node_modules"
	mkdir -p "$(DL_DIR)"
	cp -f "./"package*.json "$(DL_DIR)"
	$(NPM) install --prefix "$(DL_DIR)"
	touch "$(NODE_MODULES_STAMP)"
endef

all: build

$(NODE_MODULES_STAMP): package.json
	$(call setup_node_modules)

build: $(NODE_MODULES_STAMP)
	rm -rf "./node_modules"
	ln -s "$(DL_DIR)/node_modules" "./node_modules"
	@$(NPM) run compile

install: build
	cp -af $(FILES_DIR)/* $(DESTDIR)
	install -d $(DESTDIR)/www/views
	cp -af $(COMPILED_DIR)/* $(DESTDIR)/www/views/

clean:
	rm -rf $(COMPILED_DIR) "$(DL_DIR)/node_modules" "$(NODE_MODULES_STAMP)"

.PHONY: all build install clean
