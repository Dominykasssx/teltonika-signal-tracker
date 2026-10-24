#include "ev.h"

static struct ev_timer *timers;
static long long (*clock_fn)(void);
static void (*change_hook)(void);

void ev_set_clock(long long (*now_ms)(void))
{
	clock_fn = now_ms;
}

void ev_set_change_hook(void (*hook)(void))
{
	change_hook = hook;
}

long long ev_now(void)
{
	return clock_fn ? clock_fn() : 0;
}

static void unlink_timer(struct ev_timer *t)
{
	for (struct ev_timer **p = &timers; *p; p = &(*p)->next) {
		if (*p == t) {
			*p = t->next;
			break;
		}
	}
	t->next = NULL;
	t->pending = false;
}

void ev_timer_set(struct ev_timer *t, int ms)
{
	if (t->pending)
		unlink_timer(t);
	t->deadline = ev_now() + (ms > 0 ? ms : 0);
	t->pending = true;
	t->next = timers;
	timers = t;
	if (change_hook)
		change_hook();
}

void ev_timer_cancel(struct ev_timer *t)
{
	if (!t->pending)
		return;
	unlink_timer(t);
	if (change_hook)
		change_hook();
}

static struct ev_timer *earliest(void)
{
	struct ev_timer *best = NULL;

	for (struct ev_timer *t = timers; t; t = t->next) {
		if (!best || t->deadline < best->deadline)
			best = t;
	}
	return best;
}

int ev_next_timeout(void)
{
	struct ev_timer *t = earliest();
	long long left;

	if (!t)
		return -1;
	left = t->deadline - ev_now();
	return left > 0 ? (int)left : 0;
}

void ev_cancel_all(void)
{
	while (timers)
		unlink_timer(timers);
	if (change_hook)
		change_hook();
}

void ev_run_due(void)
{
	struct ev_timer *t;

	while ((t = earliest()) && t->deadline <= ev_now()) {
		unlink_timer(t);
		t->cb(t);
	}
}
