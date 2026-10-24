#include "sim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flatjson.h"

#define SIM_SETTLE_MS 100

/* ------------------------------------------------------------------ controller */

void sim_controller_init(struct sim_controller *d, int min_angle, int max_angle)
{
	memset(d, 0, sizeof(*d));
	d->min_angle = min_angle;
	d->max_angle = max_angle;
	d->ms_per_60deg = 150;
}

static bool sim_moving(const struct sim_controller *d, long long now)
{
	return d->output && now < d->move_end;
}

static size_t sim_error(int id, bool has_id, const char *code, char *out, size_t cap)
{
	int n = has_id ? snprintf(out, cap, "{\"id\":%d,\"status\":\"error\",\"error\":\"%s\","
				  "\"message\":\"%s\"}\n", id, code, code)
		       : snprintf(out, cap, "{\"status\":\"error\",\"error\":\"%s\","
				  "\"message\":\"%s\"}\n", code, code);
	return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

size_t sim_controller_handle(void *dev, const char *line, long long now, char *out, size_t cap)
{
	struct sim_controller *d = dev;
	struct fj_object req;
	const char *cmd;
	char angle[12];
	bool has_id = false;
	int id = 0, n, target;

	if (fj_parse(line, &req) != FJ_OK)
		return sim_error(0, false, "invalid_json", out, cap);
	has_id = fj_get_int(&req, "id", &id);
	if (!fj_get_string(&req, "command", &cmd))
		return sim_error(id, has_id, "missing_command", out, cap);

	if (d->has_angle)
		snprintf(angle, sizeof(angle), "%d", d->angle);
	else
		snprintf(angle, sizeof(angle), "null");

	if (strcmp(cmd, "get_status") == 0) {
		bool moving = sim_moving(d, now);

		n = snprintf(out, cap,
			     "{\"id\":%d,\"status\":\"ok\",\"state\":\"%s\",\"commanded_angle\":%s,"
			     "\"moving\":%s,\"output_enabled\":%s,\"interrupted\":%s,"
			     "\"position_feedback\":false,\"min_angle\":%d,\"max_angle\":%d}\n",
			     id, moving ? "moving" : (d->output ? "holding" : "released"), angle,
			     moving ? "true" : "false", d->output ? "true" : "false",
			     d->interrupted ? "true" : "false", d->min_angle, d->max_angle);
	} else if (strcmp(cmd, "set_position") == 0) {
		int move_ms;

		if (!fj_get(&req, "angle"))
			return sim_error(id, has_id, "missing_angle", out, cap);
		if (!fj_get_int(&req, "angle", &target))
			return sim_error(id, has_id, "invalid_angle", out, cap);
		if (target < d->min_angle || target > d->max_angle)
			return sim_error(id, has_id, "angle_out_of_range", out, cap);
		if (sim_moving(d, now))
			return sim_error(id, has_id, "busy", out, cap);

		if (d->has_angle && !d->interrupted)
			move_ms = abs(target - d->angle) * d->ms_per_60deg / 60;
		else
			move_ms = 180 * d->ms_per_60deg / 60;
		if (move_ms > 0 || !d->output)
			move_ms += SIM_SETTLE_MS;
		d->from_angle = d->has_angle ? d->angle : target;
		d->has_angle = true;
		d->angle = target;
		d->output = true;
		d->interrupted = false;
		d->move_start = now;
		d->move_end = now + move_ms;
		n = snprintf(out, cap,
			     "{\"id\":%d,\"status\":\"ok\",\"commanded_angle\":%d,\"moving\":%s,"
			     "\"move_time_ms\":%d}\n",
			     id, target, move_ms > 0 ? "true" : "false", move_ms);
	} else if (strcmp(cmd, "stop") == 0) {
		if (sim_moving(d, now))
			d->interrupted = true;
		d->output = false;
		d->move_end = now;
		n = snprintf(out, cap,
			     "{\"id\":%d,\"status\":\"ok\",\"commanded_angle\":%s,\"moving\":false,"
			     "\"output_enabled\":false,\"interrupted\":%s}\n",
			     id, angle, d->interrupted ? "true" : "false");
	} else {
		return sim_error(id, has_id, "unknown_command", out, cap);
	}
	return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

size_t sim_controller_boot(struct sim_controller *d, char *out, size_t cap)
{
	int n;

	d->has_angle = false;
	d->output = false;
	d->interrupted = false;
	d->move_end = 0;
	n = snprintf(out, cap,
		     "{\"event\":\"boot\",\"fw\":\"sim\",\"min_angle\":%d,\"max_angle\":%d,"
		     "\"position_feedback\":false}\n", d->min_angle, d->max_angle);
	return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

bool sim_controller_position(const struct sim_controller *d, long long now, double *angle)
{
	if (!d->has_angle)
		return false;
	if (sim_moving(d, now) && d->move_end > d->move_start) {
		double f = (double)(now - d->move_start) / (double)(d->move_end - d->move_start);

		*angle = d->from_angle + (d->angle - d->from_angle) * f;
	} else {
		*angle = d->angle;
	}
	return true;
}

/* ------------------------------------------------------------------ transport */

static void deliver(struct ev_timer *timer)
{
	struct sim_transport *t = ev_container_of(timer, struct sim_transport, deliver_timer);
	char buf[sizeof(t->outbox)];
	size_t len = t->outbox_len;

	if (!t->connected || len == 0)
		return;
	memcpy(buf, t->outbox, len);
	t->outbox_len = 0;
	esp_link_feed(t->link, buf, len);
}

static void queue_out(struct sim_transport *t, const char *data, size_t len)
{
	if (t->outbox_len + len > sizeof(t->outbox))
		return;
	memcpy(t->outbox + t->outbox_len, data, len);
	t->outbox_len += len;
	if (!t->deliver_timer.pending)
		ev_timer_set(&t->deliver_timer, t->latency_ms);
}

static void on_request_line(void *ctx, const char *line)
{
	struct sim_transport *t = ctx;
	char resp[320];
	size_t n;

	if (!line || t->mute)
		return;
	n = t->handle(t->dev, line, ev_now(), resp, sizeof(resp));
	if (n > 0)
		queue_out(t, resp, n);
}

static int sim_write(void *io, const char *buf, size_t len)
{
	struct sim_transport *t = io;

	if (!t->connected)
		return -1;
	linebuf_feed(&t->rx, buf, len, on_request_line, t);
	return 0;
}

static void on_reconnect(struct ev_timer *timer)
{
	sim_transport_connect(ev_container_of(timer, struct sim_transport, reconnect_timer));
}

static void sim_reset(void *io)
{
	struct sim_transport *t = io;

	t->resets++;
	t->connected = false;
	t->outbox_len = 0;
	ev_timer_cancel(&t->deliver_timer);
	if (t->reconnect_ms >= 0)
		ev_timer_set(&t->reconnect_timer, t->reconnect_ms);
}

const struct esp_link_ops sim_transport_ops = {
	.write = sim_write,
	.reset = sim_reset,
};

void sim_transport_init(struct sim_transport *t, struct esp_link *link, sim_handler handle,
			void *dev, int latency_ms)
{
	memset(t, 0, sizeof(*t));
	t->link = link;
	t->handle = handle;
	t->dev = dev;
	t->latency_ms = latency_ms;
	t->reconnect_ms = -1;
	t->deliver_timer.cb = deliver;
	t->reconnect_timer.cb = on_reconnect;
}

void sim_transport_connect(struct sim_transport *t)
{
	t->connected = true;
	t->outbox_len = 0;
	linebuf_reset(&t->rx);
	esp_link_connected(t->link);
}

void sim_transport_unplug(struct sim_transport *t)
{
	t->connected = false;
	t->outbox_len = 0;
	ev_timer_cancel(&t->deliver_timer);
	esp_link_disconnected(t->link, "serial device disconnected");
	if (t->reconnect_ms >= 0)
		ev_timer_set(&t->reconnect_timer, t->reconnect_ms);
}

void sim_transport_inject(struct sim_transport *t, const char *data)
{
	queue_out(t, data, strlen(data));
}

/* ------------------------------------------------------------------ signal */

void sim_signal_init(struct sim_signal *g, int best_angle, unsigned seed)
{
	memset(g, 0, sizeof(*g));
	g->best_angle = best_angle;
	g->width = 40;
	g->noise = 1;
	g->seed = seed ? seed : 1;
	snprintf(g->cell_id, sizeof(g->cell_id), "2A3F01");
}

static double noise(struct sim_signal *g)
{
	g->seed = g->seed * 1103515245u + 12345u;
	return ((double)((g->seed >> 16) & 0x7fff) / 16383.5) - 1.0; /* -1..1 */
}

static double lobe(double angle, double center, double width)
{
	double d = fmod(fabs(angle - center), 360.0);

	if (d > 180)
		d = 360 - d;
	return exp(-(d / width) * (d / width));
}

void sim_signal_measure(struct sim_signal *g, bool known, double angle, long long now,
			struct signal_sample *out)
{
	double gain;

	signal_sample_clear(out);
	out->time_ms = now;
	if (g->fail) {
		snprintf(out->error, sizeof(out->error), "simulated measurement failure");
		return;
	}

	/* Main lobe at best_angle and a weaker side lobe 90 degrees away. */
	gain = known ? fmax(lobe(angle, g->best_angle, g->width),
			    0.6 * lobe(angle, g->best_angle + 90, g->width * 0.75))
		     : 0.3;

	/* gsmctl reports whole numbers, so the simulation does too */
	out->ok = true;
	out->has_rsrp = true;
	out->rsrp = round(-115 + 35 * gain + 1.5 * g->noise * noise(g));
	out->has_rsrq = true;
	out->rsrq = round(-16 + 9 * gain + 0.7 * g->noise * noise(g));
	if (!g->no_sinr) {
		out->has_sinr = true;
		out->sinr = round(-2 + 22 * gain + 1.5 * g->noise * noise(g));
	}
	out->has_rssi = true;
	out->rssi = out->rsrp + 25;
	out->has_cell = true;
	snprintf(out->cell_id, sizeof(out->cell_id), "%s", g->cell_id);
}

static void on_sample_timer(struct ev_timer *timer)
{
	struct sim_sampler *s = ev_container_of(timer, struct sim_sampler, timer);
	struct signal_sample out;
	double angle = 0;
	bool known = sim_controller_position(s->dev, ev_now(), &angle);

	s->busy = false;
	sim_signal_measure(&s->signal, known, angle, ev_now(), &out);
	s->done(s->ctx, &out);
}

void sim_sampler_init(struct sim_sampler *s, struct sim_controller *dev, int best_angle,
		      void (*done)(void *ctx, const struct signal_sample *s), void *ctx)
{
	memset(s, 0, sizeof(*s));
	sim_signal_init(&s->signal, best_angle, 12345);
	s->dev = dev;
	s->delay_ms = 300;
	s->timer.cb = on_sample_timer;
	s->done = done;
	s->ctx = ctx;
}

int sim_sampler_start(void *sampler)
{
	struct sim_sampler *s = sampler;

	if (s->busy)
		return -1;
	s->busy = true;
	ev_timer_set(&s->timer, s->delay_ms);
	return 0;
}
