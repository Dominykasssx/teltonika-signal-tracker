#include "esp_link.h"

#include <stdio.h>
#include <string.h>

#define PROBE_TIMEOUT_MS 2000 /* covers an ESP32 reboot triggered by opening the port */
#define MAX_CONSECUTIVE_TIMEOUTS 2
#define KEEPALIVE_MS 5000
#define MAX_REQUEST_ID 0x7fffffff

static void send_next(struct esp_link *l);

static void notify(struct esp_link *l)
{
	if (l->on_change)
		l->on_change(l->owner);
}

static void drop(struct esp_link *l, const char *reason)
{
	if (l->state == ESP_LINK_DOWN)
		return;
	esp_link_disconnected(l, reason);
	l->ops->reset(l->io);
}

static void update_status(struct esp_link *l, const struct fj_object *o)
{
	struct esp_status *s = &l->status;
	const char *fw;
	bool b;
	int v, min, max;

	if (fj_get_int(o, "commanded_angle", &v)) {
		s->has_angle = true;
		s->commanded_angle = v;
	} else if (fj_is_null(o, "commanded_angle")) {
		s->has_angle = false;
	}
	if (fj_get_bool(o, "moving", &b))
		s->moving = b;
	if (fj_get_bool(o, "output_enabled", &b))
		s->output_enabled = b;
	if (fj_get_bool(o, "interrupted", &b))
		s->interrupted = b;
	if (fj_get_bool(o, "position_feedback", &b))
		s->position_feedback = b;
	if (fj_get_int(o, "min_angle", &min) && fj_get_int(o, "max_angle", &max)) {
		s->has_limits = true;
		s->min_angle = min;
		s->max_angle = max;
	}
	if (fj_get_string(o, "fw", &fw))
		snprintf(s->fw, sizeof(s->fw), "%s", fw);
}

static void finish(struct esp_link *l, int err, const struct fj_object *resp)
{
	struct esp_request req = l->inflight;

	l->inflight.used = false;
	ev_timer_cancel(&l->req_timer);
	if (req.cb)
		req.cb(req.ctx, err, resp);
	send_next(l);
}

static int transmit(struct esp_link *l, const struct esp_request *req)
{
	char line[96];
	int n, timeout;

	if (++l->next_id > MAX_REQUEST_ID)
		l->next_id = 1;

	if (req->has_angle)
		n = snprintf(line, sizeof(line), "{\"id\":%u,\"command\":\"%s\",\"angle\":%d}\n",
			     l->next_id, req->command, req->angle);
	else
		n = snprintf(line, sizeof(line), "{\"id\":%u,\"command\":\"%s\"}\n", l->next_id,
			     req->command);

	ev_timer_cancel(&l->keepalive_timer);
	if (l->ops->write(l->io, line, (size_t)n) != 0) {
		drop(l, "write to serial port failed");
		return ESP_E_IO;
	}

	l->inflight = *req;
	l->inflight.used = true;
	l->inflight.id = l->next_id;
	timeout = l->timeout_ms;
	if (l->state == ESP_LINK_PROBING && timeout < PROBE_TIMEOUT_MS)
		timeout = PROBE_TIMEOUT_MS;
	ev_timer_set(&l->req_timer, timeout);
	return ESP_OK;
}

static void send_next(struct esp_link *l)
{
	if (l->inflight.used || l->state == ESP_LINK_DOWN)
		return;

	if (l->queued.used) {
		struct esp_request req = l->queued;

		l->queued.used = false;
		if (transmit(l, &req) != ESP_OK && req.cb)
			req.cb(req.ctx, ESP_E_IO, NULL);
		return;
	}

	if (l->state == ESP_LINK_UP)
		ev_timer_set(&l->keepalive_timer, l->keepalive_ms);
}

static int submit(struct esp_link *l, const char *command, bool has_angle, int angle,
		  esp_done_cb cb, void *ctx, bool internal)
{
	struct esp_request req;
	bool is_stop = strcmp(command, "stop") == 0;

	if (l->state == ESP_LINK_DOWN || (l->state == ESP_LINK_PROBING && !internal))
		return ESP_E_NOT_CONNECTED;

	memset(&req, 0, sizeof(req));
	req.used = true;
	req.internal = internal;
	snprintf(req.command, sizeof(req.command), "%s", command);
	req.has_angle = has_angle;
	req.angle = angle;
	req.cb = cb;
	req.ctx = ctx;

	if (!l->inflight.used)
		return transmit(l, &req);

	if (l->queued.used) {
		struct esp_request old = l->queued;

		if (!is_stop || strcmp(old.command, "stop") == 0)
			return ESP_E_BUSY;
		/* stop pre-empts a request that is still waiting behind a keepalive */
		l->queued = req;
		if (old.cb)
			old.cb(old.ctx, ESP_E_BUSY, NULL);
		return ESP_OK;
	}

	if (l->inflight.internal || is_stop) {
		l->queued = req;
		return ESP_OK;
	}
	return ESP_E_BUSY;
}

static void internal_done(void *ctx, int err, const struct fj_object *resp)
{
	struct esp_link *l = ctx;

	(void)resp;
	if (err == ESP_OK && l->state == ESP_LINK_PROBING) {
		l->state = ESP_LINK_UP;
		l->last_error[0] = '\0';
		notify(l);
	}
}

static void on_req_timeout(struct ev_timer *t)
{
	struct esp_link *l = ev_container_of(t, struct esp_link, req_timer);
	bool probing = l->state == ESP_LINK_PROBING;
	bool give_up;

	if (!l->inflight.used)
		return;

	l->n_timeouts++;
	l->consecutive_timeouts++;
	snprintf(l->last_error, sizeof(l->last_error), "controller did not answer '%s'",
		 l->inflight.command);
	give_up = probing || l->consecutive_timeouts >= MAX_CONSECUTIVE_TIMEOUTS;

	finish(l, ESP_E_TIMEOUT, NULL);
	if (give_up)
		drop(l, probing ? "controller did not answer after connecting"
				: "controller stopped responding");
}

static void on_keepalive(struct ev_timer *t)
{
	struct esp_link *l = ev_container_of(t, struct esp_link, keepalive_timer);

	if (l->state == ESP_LINK_UP && !l->inflight.used && !l->queued.used)
		submit(l, "get_status", false, 0, internal_done, l, true);
}

static void handle_boot(struct esp_link *l, const struct fj_object *o)
{
	struct esp_status *s = &l->status;
	bool was_probing = l->state == ESP_LINK_PROBING;

	/* A rebooted controller has lost its commanded position and drives no PWM. */
	s->known = true;
	s->has_angle = false;
	s->moving = false;
	s->output_enabled = false;
	s->interrupted = false;
	update_status(l, o);

	if (l->inflight.used) {
		struct esp_request req = l->inflight;

		l->inflight.used = false;
		ev_timer_cancel(&l->req_timer);
		if (!req.internal && req.cb)
			req.cb(req.ctx, ESP_E_DISCONNECTED, NULL);
	}

	l->consecutive_timeouts = 0;
	if (was_probing) {
		/* Opening the port often resets dev boards (DTR/RTS); this is a normal connect. */
		l->state = ESP_LINK_UP;
		l->last_error[0] = '\0';
	} else {
		snprintf(l->last_error, sizeof(l->last_error), "controller rebooted");
	}

	if (l->on_reboot && !was_probing)
		l->on_reboot(l->owner);
	notify(l);
	send_next(l);
}

static void handle_response(struct esp_link *l, const struct fj_object *o)
{
	const char *status = NULL, *code = "unknown", *message = "";

	l->consecutive_timeouts = 0;
	update_status(l, o);
	fj_get_string(o, "status", &status);

	if (status && strcmp(status, "ok") == 0) {
		if (strcmp(l->inflight.command, "get_status") == 0)
			l->status.known = true;
		if (strcmp(l->inflight.command, "set_position") == 0) {
			l->status.output_enabled = true;
			l->status.interrupted = false;
		}
		finish(l, ESP_OK, o);
		return;
	}

	fj_get_string(o, "error", &code);
	fj_get_string(o, "message", &message);
	snprintf(l->last_error, sizeof(l->last_error), "controller rejected '%s': %s (%s)",
		 l->inflight.command, message, code);
	finish(l, ESP_E_REMOTE, o);
}

static void on_line(void *ctx, const char *line)
{
	struct esp_link *l = ctx;
	struct fj_object o;
	const char *event, *status;
	int id;

	if (!line) {
		l->n_malformed++;
		return;
	}
	while (*line == ' ' || *line == '\t')
		line++;
	if (*line != '{') {
		l->n_noise++; /* boot ROM banner or other non-protocol output */
		return;
	}
	if (fj_parse(line, &o) != FJ_OK) {
		l->n_malformed++;
		return;
	}

	if (fj_get_string(&o, "event", &event)) {
		if (strcmp(event, "boot") == 0)
			handle_boot(l, &o);
		return;
	}

	if (!fj_get_int(&o, "id", &id)) {
		/* The controller cannot echo the id of a request it failed to parse. With a
		 * single request in flight, such an error belongs to that request. */
		if (l->inflight.used && fj_get_string(&o, "status", &status) &&
		    strcmp(status, "error") == 0)
			handle_response(l, &o);
		else
			l->n_malformed++;
		return;
	}

	if (!l->inflight.used || (unsigned)id != l->inflight.id) {
		l->n_stale++; /* late answer to a request that already timed out */
		return;
	}
	handle_response(l, &o);
}

void esp_link_init(struct esp_link *l, const struct esp_link_ops *ops, void *io, int timeout_ms)
{
	memset(l, 0, sizeof(*l));
	l->ops = ops;
	l->io = io;
	l->timeout_ms = timeout_ms;
	l->keepalive_ms = KEEPALIVE_MS;
	l->req_timer.cb = on_req_timeout;
	l->keepalive_timer.cb = on_keepalive;
	linebuf_reset(&l->lb);
}

void esp_link_set_events(struct esp_link *l, void (*on_change)(void *owner),
			 void (*on_reboot)(void *owner), void *owner)
{
	l->on_change = on_change;
	l->on_reboot = on_reboot;
	l->owner = owner;
}

void esp_link_connected(struct esp_link *l)
{
	if (l->state != ESP_LINK_DOWN)
		esp_link_disconnected(l, NULL);

	l->state = ESP_LINK_PROBING;
	l->consecutive_timeouts = 0;
	l->status.known = false;
	linebuf_reset(&l->lb);
	notify(l);
	submit(l, "get_status", false, 0, internal_done, l, true);
}

void esp_link_disconnected(struct esp_link *l, const char *reason)
{
	struct esp_request a = l->inflight, b = l->queued;
	bool was_down = l->state == ESP_LINK_DOWN;

	l->state = ESP_LINK_DOWN;
	l->inflight.used = false;
	l->queued.used = false;
	ev_timer_cancel(&l->req_timer);
	ev_timer_cancel(&l->keepalive_timer);
	linebuf_reset(&l->lb);
	l->status.known = false;
	l->status.moving = false;
	if (reason)
		snprintf(l->last_error, sizeof(l->last_error), "%s", reason);

	if (a.used && a.cb)
		a.cb(a.ctx, ESP_E_DISCONNECTED, NULL);
	if (b.used && b.cb)
		b.cb(b.ctx, ESP_E_DISCONNECTED, NULL);
	if (!was_down)
		notify(l);
}

void esp_link_feed(struct esp_link *l, const char *data, size_t n)
{
	if (l->state != ESP_LINK_DOWN)
		linebuf_feed(&l->lb, data, n, on_line, l);
}

int esp_link_get_status(struct esp_link *l, esp_done_cb cb, void *ctx)
{
	return submit(l, "get_status", false, 0, cb, ctx, false);
}

int esp_link_set_position(struct esp_link *l, int angle, esp_done_cb cb, void *ctx)
{
	return submit(l, "set_position", true, angle, cb, ctx, false);
}

int esp_link_stop(struct esp_link *l, esp_done_cb cb, void *ctx)
{
	return submit(l, "stop", false, 0, cb, ctx, false);
}

bool esp_link_idle(const struct esp_link *l)
{
	return l->state == ESP_LINK_UP && (!l->inflight.used || l->inflight.internal) &&
	       !l->queued.used;
}

const char *esp_link_state_name(enum esp_link_state st)
{
	switch (st) {
	case ESP_LINK_DOWN: return "disconnected";
	case ESP_LINK_PROBING: return "connecting";
	case ESP_LINK_UP: return "connected";
	}
	return "unknown";
}

const char *esp_err_name(int err)
{
	switch (err) {
	case ESP_OK: return "ok";
	case ESP_E_REMOTE: return "rejected by controller";
	case ESP_E_TIMEOUT: return "controller timeout";
	case ESP_E_DISCONNECTED: return "controller disconnected";
	case ESP_E_BUSY: return "controller busy";
	case ESP_E_NOT_CONNECTED: return "controller not connected";
	case ESP_E_IO: return "serial write failed";
	}
	return "unknown error";
}
