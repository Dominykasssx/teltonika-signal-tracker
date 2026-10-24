/*
 * Request/response link to the ESP32 servo controller.
 *
 * Owns the protocol state: one request in flight at a time, response matching by id,
 * request timeouts, keepalive probes, controller reboot detection and status tracking.
 * The byte transport (serial port or simulator) is reached through struct esp_link_ops.
 */
#ifndef ESP_LINK_H
#define ESP_LINK_H

#include <stdbool.h>
#include <stddef.h>

#include "ev.h"
#include "flatjson.h"
#include "linebuf.h"

enum esp_link_state {
	ESP_LINK_DOWN,    /* no transport */
	ESP_LINK_PROBING, /* transport open, waiting for the controller to answer */
	ESP_LINK_UP,
};

enum esp_err {
	ESP_OK = 0,
	ESP_E_REMOTE,        /* controller answered with an error */
	ESP_E_TIMEOUT,
	ESP_E_DISCONNECTED,  /* link dropped or controller rebooted while waiting */
	ESP_E_BUSY,          /* another request is already in flight */
	ESP_E_NOT_CONNECTED,
	ESP_E_IO,
};

/* resp is the parsed response line for ESP_OK and ESP_E_REMOTE, otherwise NULL. */
typedef void (*esp_done_cb)(void *ctx, int err, const struct fj_object *resp);

struct esp_status {
	bool known;            /* a status was received on the current connection */
	bool has_angle;        /* controller reports a commanded angle (not after reboot) */
	int commanded_angle;
	bool moving;
	bool output_enabled;
	bool interrupted;      /* last move was stopped early: actual position unknown */
	bool position_feedback;
	bool has_limits;
	int min_angle;
	int max_angle;
	char fw[16];
};

struct esp_link_ops {
	int (*write)(void *io, const char *buf, size_t len); /* 0 on success */
	void (*reset)(void *io); /* close the transport; it reconnects on its own schedule */
};

struct esp_request {
	bool used;
	bool internal; /* probe or keepalive issued by the link itself */
	unsigned id;
	char command[16];
	bool has_angle;
	int angle;
	esp_done_cb cb;
	void *ctx;
};

struct esp_link {
	const struct esp_link_ops *ops;
	void *io;
	void (*on_change)(void *owner);
	void (*on_reboot)(void *owner);
	void *owner;

	enum esp_link_state state;
	struct linebuf lb;
	int timeout_ms;
	int keepalive_ms;
	unsigned next_id;
	struct esp_request inflight;
	struct esp_request queued; /* one slot: stop, or a request waiting behind a keepalive */
	struct ev_timer req_timer;
	struct ev_timer keepalive_timer;
	int consecutive_timeouts;

	struct esp_status status;
	char last_error[128];
	unsigned long n_noise;
	unsigned long n_malformed;
	unsigned long n_timeouts;
	unsigned long n_stale;
};

void esp_link_init(struct esp_link *l, const struct esp_link_ops *ops, void *io,
		   int timeout_ms);
void esp_link_set_events(struct esp_link *l, void (*on_change)(void *owner),
			 void (*on_reboot)(void *owner), void *owner);

/* Transport notifications. */
void esp_link_connected(struct esp_link *l);
void esp_link_disconnected(struct esp_link *l, const char *reason);
void esp_link_feed(struct esp_link *l, const char *data, size_t n);

/*
 * Requests return ESP_OK when accepted; the callback then runs exactly once, later.
 * Any other return value means the request was not sent and the callback never runs.
 */
int esp_link_get_status(struct esp_link *l, esp_done_cb cb, void *ctx);
int esp_link_set_position(struct esp_link *l, int angle, esp_done_cb cb, void *ctx);
int esp_link_stop(struct esp_link *l, esp_done_cb cb, void *ctx);

bool esp_link_idle(const struct esp_link *l);
const char *esp_link_state_name(enum esp_link_state st);
const char *esp_err_name(int err);

#endif
