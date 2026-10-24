/*
 * One servo move at a time: send set_position, then poll get_status until the
 * controller reports the move finished or the movement timeout expires.
 */
#ifndef MOTION_H
#define MOTION_H

#include <stdbool.h>

#include "esp_link.h"
#include "ev.h"

#define MOTION_POLL_MS 200

/* err is an enum esp_err value, or MOTION_E_TIMEOUT / MOTION_E_STOPPED. */
typedef void (*motion_done_cb)(void *ctx, int err, const char *msg);

enum {
	MOTION_E_TIMEOUT = 100,
	MOTION_E_STOPPED,
};

struct motion {
	struct esp_link *link;
	bool active;
	int target;
	unsigned gen;     /* bumped on abort so late replies are ignored */
	unsigned req_gen; /* generation of the request currently with the link */
	long long deadline;
	int timeout_ms;
	struct ev_timer poll_timer;
	motion_done_cb done;
	void *ctx;
	char error[128];
};

void motion_init(struct motion *m, struct esp_link *link);
/* Returns ESP_OK if the move was sent; done() then runs exactly once unless aborted. */
int motion_move(struct motion *m, int angle, int timeout_ms, motion_done_cb done, void *ctx);
/* Stops tracking the current move without calling done(). Does not stop the servo. */
void motion_abort(struct motion *m);

#endif
