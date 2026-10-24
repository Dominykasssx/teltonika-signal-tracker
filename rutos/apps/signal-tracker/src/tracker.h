/*
 * Application core: validates user commands, coordinates manual moves and scans, shares
 * the single signal sampler between the scan and the idle status poll, and reacts to
 * controller disconnects and reboots. Platform independent; main.c supplies the serial
 * transport, the gsmctl sampler and the ubus interface.
 */
#ifndef TRACKER_H
#define TRACKER_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "esp_link.h"
#include "motion.h"
#include "scan.h"
#include "signal.h"

#define TRACKER_POLL_MS 5000

enum tracker_log_level {
	TRACKER_LOG_ERR = 3,
	TRACKER_LOG_WARN = 4,
	TRACKER_LOG_INFO = 6,
};

/* Measurement backend: start() begins one measurement and returns 0; the result is
 * reported later with tracker_sample_done(). */
struct tracker_sampler {
	int (*start)(void *ctx);
	void *ctx;
};

enum tracker_sample_origin {
	TRACKER_SAMPLE_NONE,
	TRACKER_SAMPLE_POLL,
	TRACKER_SAMPLE_SCAN,
};

struct tracker {
	struct st_config cfg;
	bool cfg_ok;
	char config_error[160];

	struct esp_link link;
	struct motion motion;
	struct scan scan;
	enum esp_link_state last_link_state;
	enum scan_state last_scan_state;
	enum scan_phase last_scan_phase;

	struct tracker_sampler sampler;
	bool sampling;
	enum tracker_sample_origin sample_origin;
	bool scan_sample_queued;
	unsigned scan_sample_token;
	unsigned scan_move_token;
	struct ev_timer poll_timer;

	struct signal_sample last_signal;
	bool has_signal;
	char last_move_error[160];

	void (*log)(int level, const char *msg);
};

void tracker_init(struct tracker *t, const struct esp_link_ops *ops, void *io,
		  const struct tracker_sampler *sampler);
/* error is NULL for a valid configuration; otherwise moves and scans are refused. */
void tracker_set_config(struct tracker *t, const struct st_config *cfg, const char *error);
/* Starts the periodic signal poll used while no scan is running. */
void tracker_start_polling(struct tracker *t);

/* Commands return NULL on success, or an error code with a message in msg. */
const char *tracker_move(struct tracker *t, int angle, char *msg, size_t len);
const char *tracker_stop(struct tracker *t, char *msg, size_t len);
const char *tracker_scan_start(struct tracker *t, char *msg, size_t len);
const char *tracker_scan_cancel(struct tracker *t, char *msg, size_t len);

void tracker_sample_done(struct tracker *t, const struct signal_sample *s);

#endif
