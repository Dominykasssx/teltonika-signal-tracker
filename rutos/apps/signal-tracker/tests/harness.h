/* Minimal test harness: CHECK macro and a fake clock driving the ev scheduler. */
#ifndef HARNESS_H
#define HARNESS_H

#include <stdio.h>

#include "ev.h"

extern int test_failures;
extern int test_checks;
extern long long test_now;

#define CHECK(cond)                                                            \
	do {                                                                   \
		test_checks++;                                                 \
		if (!(cond)) {                                                 \
			test_failures++;                                       \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                              \
	} while (0)

void harness_init(void);
/* Advances the fake clock by ms, firing timers in deadline order. */
void advance(long long ms);
int harness_report(void);

#endif
