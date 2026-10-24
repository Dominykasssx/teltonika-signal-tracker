#include "tracker.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void logf_(struct tracker *t, int level, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	if (!t->log)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	t->log(level, buf);
}

static const char *refuse(char *msg, size_t len, const char *code, const char *text)
{
	snprintf(msg, len, "%s", text);
	return code;
}

/* ------------------------------------------------------------------ sampling */

static int start_sampling(struct tracker *t, enum tracker_sample_origin origin)
{
	t->sampling = true;
	t->sample_origin = origin;
	if (t->sampler.start(t->sampler.ctx) != 0) {
		t->sampling = false;
		t->sample_origin = TRACKER_SAMPLE_NONE;
		return -1;
	}
	return 0;
}

static void deliver_failed_scan_sample(struct tracker *t, const char *why)
{
	struct signal_sample bad;

	signal_sample_clear(&bad);
	snprintf(bad.error, sizeof(bad.error), "%s", why);
	scan_sample_done(&t->scan, t->scan_sample_token, &bad);
}

void tracker_sample_done(struct tracker *t, const struct signal_sample *s)
{
	enum tracker_sample_origin origin = t->sample_origin;

	t->sampling = false;
	t->sample_origin = TRACKER_SAMPLE_NONE;
	t->last_signal = *s;
	t->has_signal = true;

	if (origin == TRACKER_SAMPLE_SCAN)
		scan_sample_done(&t->scan, t->scan_sample_token, s);

	if (t->scan_sample_queued && !t->sampling) {
		t->scan_sample_queued = false;
		if (start_sampling(t, TRACKER_SAMPLE_SCAN) != 0)
			deliver_failed_scan_sample(t, "measurement could not be started");
	}
}

static void on_poll(struct ev_timer *timer)
{
	struct tracker *t = ev_container_of(timer, struct tracker, poll_timer);

	if (!scan_running(&t->scan) && !t->sampling)
		start_sampling(t, TRACKER_SAMPLE_POLL);
	ev_timer_set(&t->poll_timer, TRACKER_POLL_MS);
}

void tracker_start_polling(struct tracker *t)
{
	ev_timer_set(&t->poll_timer, 0);
}

/* ------------------------------------------------------------------ scan glue */

static void on_scan_move_done(void *ctx, int err, const char *msg)
{
	struct tracker *t = ctx;

	scan_move_done(&t->scan, t->scan_move_token, err, msg);
}

static int scan_op_move(struct scan *s, unsigned token, int angle, int timeout_ms)
{
	struct tracker *t = s->ctx;

	t->scan_move_token = token;
	return motion_move(&t->motion, angle, timeout_ms, on_scan_move_done, t) == ESP_OK ? 0 : -1;
}

static int scan_op_sample(struct scan *s, unsigned token)
{
	struct tracker *t = s->ctx;

	t->scan_sample_token = token;
	if (t->sampling) {
		/* an idle poll is running; take the scan sample right after it */
		t->scan_sample_queued = true;
		return 0;
	}
	return start_sampling(t, TRACKER_SAMPLE_SCAN);
}

static void scan_op_changed(struct scan *s)
{
	struct tracker *t = s->ctx;

	if (s->state != t->last_scan_state) {
		t->last_scan_state = s->state;
		if (s->state == SCAN_RUNNING)
			logf_(t, TRACKER_LOG_INFO, "scan started (%d..%d, coarse %d, fine %d)",
			      s->p.min_angle, s->p.max_angle, s->p.coarse_step, s->p.fine_step);
		else
			logf_(t, s->state == SCAN_FAILED ? TRACKER_LOG_WARN : TRACKER_LOG_INFO,
			      "scan %s: %s", scan_state_name(s->state), s->message);
		if (s->state != SCAN_RUNNING)
			t->scan_sample_queued = false;
	}
	if (s->state == SCAN_RUNNING && s->phase != t->last_scan_phase) {
		t->last_scan_phase = s->phase;
		logf_(t, TRACKER_LOG_INFO, "scan phase: %s", scan_phase_name(s->phase));
	}
}

static const struct scan_ops tracker_scan_ops = {
	.move = scan_op_move,
	.sample = scan_op_sample,
	.changed = scan_op_changed,
};

/* ------------------------------------------------------------------ controller events */

static void on_link_change(void *owner)
{
	struct tracker *t = owner;
	enum esp_link_state st = t->link.state, prev = t->last_link_state;

	if (st == prev)
		return;
	t->last_link_state = st;

	if (st == ESP_LINK_UP)
		logf_(t, TRACKER_LOG_INFO, "controller connected (fw %s)",
		      t->link.status.fw[0] ? t->link.status.fw : "unknown");
	else if (prev == ESP_LINK_UP)
		logf_(t, TRACKER_LOG_WARN, "controller disconnected: %s", t->link.last_error);

	if (st != ESP_LINK_UP && scan_running(&t->scan))
		scan_fail(&t->scan, "Controller disconnected during the scan");
}

static void on_link_reboot(void *owner)
{
	struct tracker *t = owner;

	logf_(t, TRACKER_LOG_WARN, "controller rebooted; commanded position lost");
	if (scan_running(&t->scan))
		scan_fail(&t->scan, "Controller rebooted during the scan; position is unknown");
	if (t->motion.active) {
		motion_abort(&t->motion);
		snprintf(t->last_move_error, sizeof(t->last_move_error),
			 "Controller rebooted during the move");
	}
}

/* ------------------------------------------------------------------ commands */

static void on_manual_done(void *ctx, int err, const char *msg)
{
	struct tracker *t = ctx;

	if (err) {
		snprintf(t->last_move_error, sizeof(t->last_move_error), "%s", msg);
		logf_(t, TRACKER_LOG_WARN, "manual move to %d failed: %s", t->motion.target, msg);
	} else {
		logf_(t, TRACKER_LOG_INFO, "manual move to %d done", t->motion.target);
	}
}

static const char *check_controller(struct tracker *t, char *msg, size_t len)
{
	if (!t->cfg_ok)
		return refuse(msg, len, "config_invalid", t->config_error);
	if (t->link.state != ESP_LINK_UP)
		return refuse(msg, len, "not_connected", "Servo controller is not connected");
	return NULL;
}

const char *tracker_move(struct tracker *t, int angle, char *msg, size_t len)
{
	const struct esp_status *st = &t->link.status;
	const char *code;
	int err;

	if (!t->cfg_ok)
		return refuse(msg, len, "config_invalid", t->config_error);
	if (scan_running(&t->scan))
		return refuse(msg, len, "scan_running", "Cancel the scan before moving manually");
	if (angle < t->cfg.min_angle || angle > t->cfg.max_angle) {
		snprintf(msg, len, "Angle must be between %d and %d", t->cfg.min_angle,
			 t->cfg.max_angle);
		return "out_of_range";
	}
	if (st->has_limits && (angle < st->min_angle || angle > st->max_angle)) {
		snprintf(msg, len, "Angle is outside the controller limits (%d..%d)",
			 st->min_angle, st->max_angle);
		return "out_of_range";
	}
	if ((code = check_controller(t, msg, len)))
		return code;
	if (t->motion.active)
		return refuse(msg, len, "busy", "A move is already in progress");

	t->last_move_error[0] = '\0';
	err = motion_move(&t->motion, angle, t->cfg.move_timeout_ms, on_manual_done, t);
	if (err != ESP_OK) {
		snprintf(msg, len, "%s", esp_err_name(err));
		return err == ESP_E_BUSY ? "busy" : "controller_error";
	}
	logf_(t, TRACKER_LOG_INFO, "manual move to %d", angle);
	return NULL;
}

static void on_stop_done(void *ctx, int err, const struct fj_object *resp)
{
	struct tracker *t = ctx;

	(void)resp;
	if (err) {
		snprintf(t->last_move_error, sizeof(t->last_move_error), "Stop failed: %s",
			 t->link.last_error[0] ? t->link.last_error : esp_err_name(err));
		logf_(t, TRACKER_LOG_WARN, "%s", t->last_move_error);
	}
}

const char *tracker_stop(struct tracker *t, char *msg, size_t len)
{
	int err;

	scan_cancel(&t->scan, "Stopped by user");
	motion_abort(&t->motion);
	if (t->link.state != ESP_LINK_UP)
		return refuse(msg, len, "not_connected", "Servo controller is not connected");

	err = esp_link_stop(&t->link, on_stop_done, t);
	if (err != ESP_OK) {
		snprintf(msg, len, "%s", esp_err_name(err));
		return "controller_error";
	}
	logf_(t, TRACKER_LOG_INFO, "stop requested");
	return NULL;
}

const char *tracker_scan_start(struct tracker *t, char *msg, size_t len)
{
	const struct esp_status *st = &t->link.status;
	struct scan_params p;
	const char *code;
	bool start_known;

	if (scan_running(&t->scan))
		return refuse(msg, len, "scan_running", "A scan is already running");
	if ((code = check_controller(t, msg, len)))
		return code;
	if (t->motion.active)
		return refuse(msg, len, "busy", "Wait for the current move to finish");
	if (st->has_limits && (t->cfg.min_angle < st->min_angle || t->cfg.max_angle > st->max_angle)) {
		snprintf(msg, len, "Configured range %d..%d exceeds the controller limits %d..%d",
			 t->cfg.min_angle, t->cfg.max_angle, st->min_angle, st->max_angle);
		return "out_of_range";
	}

	p.min_angle = t->cfg.min_angle;
	p.max_angle = t->cfg.max_angle;
	p.coarse_step = t->cfg.coarse_step;
	p.fine_step = t->cfg.fine_step;
	p.settle_ms = t->cfg.settle_ms;
	p.samples = t->cfg.samples;
	p.sample_interval_ms = t->cfg.sample_interval_ms;
	p.min_valid = t->cfg.min_valid_samples;
	p.move_timeout_ms = t->cfg.move_timeout_ms;
	p.min_improvement = t->cfg.min_improvement;
	p.weights = t->cfg.weights;

	/* Only a completed, still-driven move counts as a known start position. */
	start_known = st->known && st->has_angle && st->output_enabled && !st->interrupted;
	t->last_move_error[0] = '\0';
	scan_start(&t->scan, &p, start_known, st->commanded_angle);
	return NULL;
}

const char *tracker_scan_cancel(struct tracker *t, char *msg, size_t len)
{
	if (!scan_running(&t->scan))
		return refuse(msg, len, "not_running", "No scan is running");
	scan_cancel(&t->scan, "Cancelled by user");
	return NULL;
}

/* ------------------------------------------------------------------ setup */

void tracker_init(struct tracker *t, const struct esp_link_ops *ops, void *io,
		  const struct tracker_sampler *sampler)
{
	memset(t, 0, sizeof(*t));
	config_defaults(&t->cfg);
	t->cfg_ok = true;
	esp_link_init(&t->link, ops, io, t->cfg.request_timeout_ms);
	esp_link_set_events(&t->link, on_link_change, on_link_reboot, t);
	motion_init(&t->motion, &t->link);
	scan_init(&t->scan, &tracker_scan_ops, t);
	t->sampler = *sampler;
	t->poll_timer.cb = on_poll;
	t->last_link_state = ESP_LINK_DOWN;
}

void tracker_set_config(struct tracker *t, const struct st_config *cfg, const char *error)
{
	t->cfg = *cfg;
	t->cfg_ok = error == NULL;
	snprintf(t->config_error, sizeof(t->config_error), "%s", error ? error : "");
	t->link.timeout_ms = cfg->request_timeout_ms;
}
