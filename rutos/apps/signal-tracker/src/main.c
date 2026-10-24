/*
 * signal-tracker daemon: rotates the RUTX with an ESP32-driven servo to find the best
 * cellular signal. Exposes the ubus object "signal_tracker", used by the VuCI API.
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <libubox/blobmsg.h>
#include <libubox/ulog.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "config.h"
#include "ev.h"
#include "gsm.h"
#include "serial.h"
#include "sim.h"
#include "tracker.h"

static struct tracker trk;
static struct serial_port serial;
static struct gsm_sampler gsm;
static struct sim_transport sim_tr;
static struct sim_controller sim_dev;
static struct sim_sampler sim_smp;
static bool simulation;

static struct ubus_auto_conn conn;
static struct blob_buf b;
static struct uloop_timeout ev_bridge;

/* ------------------------------------------------------------------ glue */

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ev_rearm(void)
{
	int t = ev_next_timeout();

	if (t < 0)
		uloop_timeout_cancel(&ev_bridge);
	else
		uloop_timeout_set(&ev_bridge, t);
}

static void ev_bridge_cb(struct uloop_timeout *t)
{
	(void)t;
	ev_run_due();
	ev_rearm();
}

static void log_cb(int level, const char *msg)
{
	ulog(level, "%s\n", msg);
}

static void sample_done(void *ctx, const struct signal_sample *s)
{
	tracker_sample_done(ctx, s);
}

static void load_config(bool initial)
{
	static char last_err[160];
	struct st_config cfg, old = trk.cfg;
	char err[160] = "";
	int rc = config_load(&cfg, err, sizeof(err));

	tracker_set_config(&trk, &cfg, rc == 0 ? NULL : err);
	if (rc != 0 && strcmp(err, last_err) != 0)
		ULOG_ERR("configuration error: %s\n", err);
	snprintf(last_err, sizeof(last_err), "%s", rc == 0 ? "" : err);
	if (initial || simulation)
		return;
	if (strcmp(old.device, cfg.device) != 0 || old.n_usb_ids != cfg.n_usb_ids ||
	    memcmp(old.usb_ids, cfg.usb_ids, sizeof(cfg.usb_ids)) != 0)
		serial_restart(&serial);
	if (old.simulate != cfg.simulate)
		ULOG_WARN("simulation mode changes take effect after a service restart\n");
}

/*
 * Settings saved from the WebUI are picked up on the next request; a running scan keeps
 * the settings it started with.
 */
static void refresh_config(void)
{
	if (!scan_running(&trk.scan))
		load_config(false);
}

/* ------------------------------------------------------------------ status */

static double round1(double v)
{
	return round(v * 10.0) / 10.0;
}

static void add_result(const char *name, const struct angle_result *r)
{
	void *t = blobmsg_open_table(&b, name);

	blobmsg_add_u32(&b, "angle", (uint32_t)r->angle);
	blobmsg_add_u8(&b, "scored", r->scored);
	if (r->scored)
		blobmsg_add_double(&b, "score", round1(r->score));
	if (r->has_rsrp)
		blobmsg_add_double(&b, "rsrp", round1(r->rsrp));
	if (r->has_rsrq)
		blobmsg_add_double(&b, "rsrq", round1(r->rsrq));
	if (r->has_sinr)
		blobmsg_add_double(&b, "sinr", round1(r->sinr));
	blobmsg_add_u32(&b, "valid", (uint32_t)r->valid);
	blobmsg_add_u32(&b, "samples", (uint32_t)r->samples);
	if (r->has_cell)
		blobmsg_add_string(&b, "cell_id", r->cell_id);
	blobmsg_add_u8(&b, "cell_changed", r->cell_changed);
	blobmsg_close_table(&b, t);
}

static void add_controller(void)
{
	const struct esp_link *l = &trk.link;
	const char *error = l->last_error;
	void *t = blobmsg_open_table(&b, "controller");

	blobmsg_add_string(&b, "state", esp_link_state_name(l->state));
	if (simulation)
		blobmsg_add_string(&b, "device", "simulation");
	else if (serial.open)
		blobmsg_add_string(&b, "device", serial.path);
	if (l->state == ESP_LINK_DOWN && !simulation && serial.error[0])
		error = serial.error;
	if (error[0])
		blobmsg_add_string(&b, "error", error);
	if (l->status.fw[0])
		blobmsg_add_string(&b, "fw", l->status.fw);
	blobmsg_add_u8(&b, "position_feedback", l->status.position_feedback);
	if (l->status.has_limits) {
		blobmsg_add_u32(&b, "min_angle", (uint32_t)l->status.min_angle);
		blobmsg_add_u32(&b, "max_angle", (uint32_t)l->status.max_angle);
	}
	blobmsg_add_u32(&b, "timeouts", (uint32_t)l->n_timeouts);
	blobmsg_add_u32(&b, "malformed", (uint32_t)l->n_malformed);
	blobmsg_close_table(&b, t);
}

static void add_position(void)
{
	const struct esp_status *st = &trk.link.status;
	void *t = blobmsg_open_table(&b, "position");

	/* Commanded angle only: the servo gives no feedback, so there is no measured angle. */
	if (st->has_angle)
		blobmsg_add_u32(&b, "commanded_angle", (uint32_t)st->commanded_angle);
	blobmsg_add_u8(&b, "status_known", st->known);
	blobmsg_add_u8(&b, "moving", st->moving || trk.motion.active);
	blobmsg_add_u8(&b, "output_enabled", st->output_enabled);
	blobmsg_add_u8(&b, "interrupted", st->interrupted);
	blobmsg_add_u8(&b, "feedback", st->position_feedback);
	if (trk.motion.active)
		blobmsg_add_u32(&b, "move_target", (uint32_t)trk.motion.target);
	if (trk.last_move_error[0])
		blobmsg_add_string(&b, "last_error", trk.last_move_error);
	blobmsg_close_table(&b, t);
}

static void add_signal(void)
{
	const struct signal_sample *s = &trk.last_signal;
	void *t = blobmsg_open_table(&b, "signal");

	blobmsg_add_u8(&b, "available", trk.has_signal && signal_has_metrics(s));
	if (trk.has_signal) {
		if (s->has_rsrp)
			blobmsg_add_double(&b, "rsrp", s->rsrp);
		if (s->has_rsrq)
			blobmsg_add_double(&b, "rsrq", s->rsrq);
		if (s->has_sinr)
			blobmsg_add_double(&b, "sinr", s->sinr);
		if (s->has_rssi)
			blobmsg_add_double(&b, "rssi", s->rssi);
		if (s->has_cell)
			blobmsg_add_string(&b, "cell_id", s->cell_id);
		if (s->error[0])
			blobmsg_add_string(&b, "error", s->error);
		blobmsg_add_u32(&b, "age_s", (uint32_t)((ev_now() - s->time_ms) / 1000));
	}
	blobmsg_close_table(&b, t);
}

static void add_scan(void)
{
	const struct scan *s = &trk.scan;
	void *t = blobmsg_open_table(&b, "scan"), *a;

	blobmsg_add_string(&b, "state", scan_state_name(s->state));
	blobmsg_add_string(&b, "phase", scan_phase_name(s->phase));
	blobmsg_add_u32(&b, "progress", (uint32_t)scan_progress(s));
	if (scan_running(s))
		blobmsg_add_u32(&b, "current_angle", (uint32_t)s->current_angle);
	blobmsg_add_u32(&b, "samples_taken", (uint32_t)s->n_samples);
	if (s->message[0])
		blobmsg_add_string(&b, "message", s->message);
	blobmsg_add_u8(&b, "complete", s->complete);
	blobmsg_add_u32(&b, "measured", (uint32_t)s->n_results);
	blobmsg_add_u8(&b, "cell_changes", s->cell_changes);
	blobmsg_add_u8(&b, "kept_baseline", s->kept_baseline);
	if (s->best_idx >= 0)
		add_result("best", &s->results[s->best_idx]);
	if (s->baseline_idx >= 0)
		add_result("baseline", &s->results[s->baseline_idx]);
	if (s->has_final)
		blobmsg_add_u32(&b, "final_angle", (uint32_t)s->final_angle);

	a = blobmsg_open_array(&b, "results");
	for (int i = 0; i < s->n_results; i++)
		add_result(NULL, &s->results[i]);
	blobmsg_close_array(&b, a);
	blobmsg_close_table(&b, t);
}

static void add_config(void)
{
	void *t = blobmsg_open_table(&b, "config");

	blobmsg_add_u8(&b, "valid", trk.cfg_ok);
	if (!trk.cfg_ok)
		blobmsg_add_string(&b, "error", trk.config_error);
	blobmsg_add_u32(&b, "min_angle", (uint32_t)trk.cfg.min_angle);
	blobmsg_add_u32(&b, "max_angle", (uint32_t)trk.cfg.max_angle);
	blobmsg_add_u32(&b, "coarse_step", (uint32_t)trk.cfg.coarse_step);
	blobmsg_add_u32(&b, "fine_step", (uint32_t)trk.cfg.fine_step);
	blobmsg_close_table(&b, t);
}

/* ------------------------------------------------------------------ ubus */

static int reply(struct ubus_context *ctx, struct ubus_request_data *req, const char *code,
		 const char *msg)
{
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", code == NULL);
	if (code) {
		blobmsg_add_string(&b, "error", code);
		blobmsg_add_string(&b, "message", msg);
	}
	ubus_send_reply(ctx, req, b.head);
	return UBUS_STATUS_OK;
}

static int m_status(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj, (void)method, (void)msg;
	refresh_config();
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", 1);
	blobmsg_add_u8(&b, "simulation", simulation);
	add_config();
	add_controller();
	add_position();
	add_signal();
	add_scan();
	ubus_send_reply(ctx, req, b.head);
	return UBUS_STATUS_OK;
}

enum { MOVE_ANGLE, __MOVE_MAX };

static const struct blobmsg_policy move_policy[__MOVE_MAX] = {
	[MOVE_ANGLE] = { .name = "angle", .type = BLOBMSG_TYPE_UNSPEC },
};

/* Accepts the integer encodings ubus clients use (int32/int64, integral double, string). */
static bool attr_to_int(struct blob_attr *a, int *out)
{
	double d;
	char *end;
	long v;

	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT32:
		*out = (int32_t)blobmsg_get_u32(a);
		return true;
	case BLOBMSG_TYPE_INT64:
		v = (long)(int64_t)blobmsg_get_u64(a);
		if (v < -100000 || v > 100000)
			return false;
		*out = (int)v;
		return true;
	case BLOBMSG_TYPE_DOUBLE:
		d = blobmsg_get_double(a);
		if (d != floor(d) || d < -100000 || d > 100000)
			return false;
		*out = (int)d;
		return true;
	case BLOBMSG_TYPE_STRING:
		v = strtol(blobmsg_get_string(a), &end, 10);
		if (end == blobmsg_get_string(a) || *end || v < -100000 || v > 100000)
			return false;
		*out = (int)v;
		return true;
	default:
		return false;
	}
}

static int m_move(struct ubus_context *ctx, struct ubus_object *obj,
		  struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__MOVE_MAX];
	char text[160];
	int angle;

	(void)obj, (void)method;
	blobmsg_parse(move_policy, __MOVE_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MOVE_ANGLE] || !attr_to_int(tb[MOVE_ANGLE], &angle))
		return reply(ctx, req, "invalid_angle", "Angle must be an integer");
	refresh_config();
	return reply(ctx, req, tracker_move(&trk, angle, text, sizeof(text)), text);
}

static int m_stop(struct ubus_context *ctx, struct ubus_object *obj,
		  struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	char text[160];

	(void)obj, (void)method, (void)msg;
	return reply(ctx, req, tracker_stop(&trk, text, sizeof(text)), text);
}

static int m_scan_start(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	char text[160];

	(void)obj, (void)method, (void)msg;
	refresh_config();
	return reply(ctx, req, tracker_scan_start(&trk, text, sizeof(text)), text);
}

static int m_scan_cancel(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	char text[160];

	(void)obj, (void)method, (void)msg;
	return reply(ctx, req, tracker_scan_cancel(&trk, text, sizeof(text)), text);
}

static int m_reload(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj, (void)method, (void)msg;
	if (scan_running(&trk.scan))
		return reply(ctx, req, "scan_running",
			     "Configuration will be applied when the next scan starts");
	load_config(false);
	return reply(ctx, req, trk.cfg_ok ? NULL : "config_invalid", trk.config_error);
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("status", m_status),
	UBUS_METHOD("move", m_move, move_policy),
	UBUS_METHOD_NOARG("stop", m_stop),
	UBUS_METHOD_NOARG("scan_start", m_scan_start),
	UBUS_METHOD_NOARG("scan_cancel", m_scan_cancel),
	UBUS_METHOD_NOARG("reload", m_reload),
};

static struct ubus_object_type obj_type = UBUS_OBJECT_TYPE("signal_tracker", methods);

static struct ubus_object obj = {
	.name = "signal_tracker",
	.type = &obj_type,
	.methods = methods,
	.n_methods = ARRAY_SIZE(methods),
};

static void on_ubus_connect(struct ubus_context *ctx)
{
	if (ubus_add_object(ctx, &obj) != 0)
		ULOG_ERR("failed to register ubus object\n");
}

/* ------------------------------------------------------------------ main */

int main(void)
{
	struct tracker_sampler sampler;
	struct st_config initial;
	char err[160];
	char boot[256];

	ulog_open(ULOG_SYSLOG, LOG_DAEMON, "signal-tracker");
	signal(SIGPIPE, SIG_IGN);
	uloop_init();
	ev_bridge.cb = ev_bridge_cb;
	ev_set_clock(mono_ms);
	ev_set_change_hook(ev_rearm);

	config_load(&initial, err, sizeof(err)); /* only to choose the mode */
	simulation = initial.simulate;

	if (simulation) {
		sampler.start = sim_sampler_start;
		sampler.ctx = &sim_smp;
		tracker_init(&trk, &sim_transport_ops, &sim_tr, &sampler);
	} else {
		sampler.start = gsm_sampler_start;
		sampler.ctx = &gsm;
		tracker_init(&trk, &serial_ops, &serial, &sampler);
	}
	trk.log = log_cb;
	load_config(true);

	if (simulation) {
		ULOG_WARN("running in simulation mode: no servo or modem is used\n");
		sim_controller_init(&sim_dev, trk.cfg.min_angle, trk.cfg.max_angle);
		sim_sampler_init(&sim_smp, &sim_dev, trk.cfg.sim_best_angle, sample_done, &trk);
		sim_transport_init(&sim_tr, &trk.link, sim_controller_handle, &sim_dev, 20);
		sim_tr.reconnect_ms = SERIAL_RETRY_MS;
		sim_transport_connect(&sim_tr);
		sim_controller_boot(&sim_dev, boot, sizeof(boot));
		sim_transport_inject(&sim_tr, boot);
	} else {
		gsm_sampler_init(&gsm, &trk.cfg, sample_done, &trk);
		serial_init(&serial, &trk.link, &trk.cfg);
		serial.log = log_cb;
		serial_start(&serial);
	}
	tracker_start_polling(&trk);

	conn.cb = on_ubus_connect;
	ubus_auto_connect(&conn);

	uloop_run();

	ubus_auto_shutdown(&conn);
	if (!simulation)
		serial_shutdown(&serial);
	uloop_done();
	ulog_close();
	return 0;
}
