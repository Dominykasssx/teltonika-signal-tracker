/*
 * Asynchronous signal measurement through RUTOS gsmctl (router only).
 *
 * Runs `gsmctl [-O <modem>] -q` for RSSI/RSRP/RSRQ/SINR and then `gsmctl -C` for the
 * serving cell id, without blocking the event loop.
 */
#ifndef GSM_H
#define GSM_H

#include <stdbool.h>
#include <stddef.h>

#include <libubox/uloop.h>

#include "config.h"
#include "signal.h"

#define GSMCTL_PATH "/usr/sbin/gsmctl"
#define GSMCTL_TIMEOUT_MS 8000

struct gsm_sampler {
	const struct st_config *cfg;
	struct uloop_process proc;
	struct uloop_fd out;
	struct uloop_timeout timeout;
	bool running;
	bool proc_done;
	bool out_done;
	bool timed_out;
	int status;
	int stage; /* 0: signal levels, 1: cell id */
	char buf[2048];
	size_t len;
	struct signal_sample sample;
	void (*done)(void *ctx, const struct signal_sample *s);
	void *ctx;
};

void gsm_sampler_init(struct gsm_sampler *g, const struct st_config *cfg,
		      void (*done)(void *ctx, const struct signal_sample *s), void *ctx);
/* tracker_sampler start(): returns 0 if the measurement was started. */
int gsm_sampler_start(void *sampler);

#endif
