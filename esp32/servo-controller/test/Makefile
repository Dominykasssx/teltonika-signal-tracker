# Host unit tests for the firmware protocol logic (no ESP-IDF needed).
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -O1 -g

test_controller: test_controller.c ../main/controller.c ../main/flatjson.c
	$(CC) $(CFLAGS) -I../main -o $@ $^ -lm

test: test_controller
	./test_controller

clean:
	rm -f test_controller test_controller.exe

.PHONY: test clean
