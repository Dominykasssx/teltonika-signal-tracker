/*
 * Simulation mode: an emulated servo controller behind the normal esp_link protocol and a
 * synthetic signal field with a known best angle. Used by `option simulate '1'` on the
 * router and by the host tests, so the scan can be exercised without hardware.
 */
#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_link.h"
#include "ev.h"
#include "linebuf.h"
#include "signal.h"

/* Emulates the ESP32 firmware protocol (see esp32/servo-controller/main/controller.c). */
struct sim_controller {
	int min_angle;
	int max_angle;
	int ms_per_60deg;
	bool has_angle;
	int angle;
	int from_angle;
	bool output;
	bool interrupted;
	long long move_start;
	long long move_end;
};

void sim_controller_init(struct sim_controller *d, int min_angle, int max_angle);
size_t sim_controller_handle(void *dev, const char *line, long long now, char *out, size_t cap);
size_t sim_controller_boot(struct sim_controller *d, char *out, size_t cap);
/* Approximate physical angle (interpolated during a move); false if never commanded. */
bool sim_controller_position(const struct sim_controller *d, long long now, double *angle);

typedef size_t (*sim_handler)(void *dev, const char *line, long long now, char *out, size_t cap);

/* In-memory byte transport between esp_link and a line handler, with response latency. */
struct sim_transport {
	struct esp_link *link;
	sim_handler handle;
	void *dev;
	int latency_ms;
	int reconnect_ms; /* < 0: stay disconnected after a reset */
	bool connected;
	bool mute; /* swallow requests, as an unresponsive controller would */
	int resets;
	struct linebuf rx;
	char outbox[1024];
	size_t outbox_len;
	struct ev_timer deliver_timer;
	struct ev_timer reconnect_timer;
};

extern const struct esp_link_ops sim_transport_ops;

void sim_transport_init(struct sim_transport *t, struct esp_link *link, sim_handler handle,
			void *dev, int latency_ms);
void sim_transport_connect(struct sim_transport *t);
void sim_transport_unplug(struct sim_transport *t);
/* Queues raw bytes for the link, e.g. a boot event or line noise. */
void sim_transport_inject(struct sim_transport *t, const char *data);

struct sim_signal {
	int best_angle;
	double width;    /* main lobe width (degrees) */
	double noise;    /* noise amplitude scale; 0 for deterministic values */
	unsigned seed;
	bool fail;     /* report a failed measurement */
	bool no_sinr;  /* modem without SINR support */
	char cell_id[SIGNAL_CELL_MAX];
};

void sim_signal_init(struct sim_signal *g, int best_angle, unsigned seed);
void sim_signal_measure(struct sim_signal *g, bool known, double angle, long long now,
			struct signal_sample *out);

/* Asynchronous measurement backend: signal at the simulated controller's position. */
struct sim_sampler {
	struct sim_signal signal;
	struct sim_controller *dev;
	int delay_ms;
	bool busy;
	struct ev_timer timer;
	void (*done)(void *ctx, const struct signal_sample *s);
	void *ctx;
};

void sim_sampler_init(struct sim_sampler *s, struct sim_controller *dev, int best_angle,
		      void (*done)(void *ctx, const struct signal_sample *s), void *ctx);
int sim_sampler_start(void *sampler);

#endif
