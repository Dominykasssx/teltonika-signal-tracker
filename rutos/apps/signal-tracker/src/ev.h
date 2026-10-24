/*
 * Tiny timer scheduler used by the protocol, motion and scan state machines.
 *
 * The core logic uses these timers instead of uloop directly so the same code runs
 * under a fake clock in host tests. On the router, main.c drives ev_run_due() from a
 * single uloop timeout re-armed through the change hook.
 */
#ifndef EV_H
#define EV_H

#include <stdbool.h>
#include <stddef.h>

#define ev_container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

struct ev_timer {
	void (*cb)(struct ev_timer *t);
	long long deadline;
	bool pending;
	struct ev_timer *next;
};

void ev_set_clock(long long (*now_ms)(void));
void ev_set_change_hook(void (*hook)(void));
long long ev_now(void);

void ev_timer_set(struct ev_timer *t, int ms);
void ev_timer_cancel(struct ev_timer *t);
/* Milliseconds until the earliest pending timer, or -1 if none is pending. */
int ev_next_timeout(void);
/* Runs all timers whose deadline has passed, including ones armed by callbacks. */
void ev_run_due(void);
/* Drops every pending timer (used when tearing down state, e.g. between tests). */
void ev_cancel_all(void);

#endif
