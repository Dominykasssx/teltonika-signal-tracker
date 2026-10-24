# Host unit tests for the daemon core (no RUTOS SDK needed): make -C tests test
CC ?= cc
CFLAGS ?= -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -O1 -g

SRC_DIR := ../src
FW_DIR := ../../../../esp32/servo-controller/main
CORE := $(addprefix $(SRC_DIR)/,ev.c flatjson.c linebuf.c esp_link.c motion.c signal.c \
	score.c scan.c config.c tracker.c sim.c)
TESTS := harness.c test_main.c test_units.c test_scenarios.c

# When run inside this repository, also test against the real firmware protocol code.
ifneq ($(wildcard $(FW_DIR)/controller.c),)
TESTS += test_firmware.c $(FW_DIR)/controller.c
CFLAGS += -DWITH_FIRMWARE_TESTS -I$(FW_DIR)
endif

run_tests: $(CORE) $(TESTS)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -I. -o $@ $(CORE) $(TESTS) -lm

test: run_tests
	./run_tests

clean:
	rm -f run_tests run_tests.exe

.PHONY: test clean
