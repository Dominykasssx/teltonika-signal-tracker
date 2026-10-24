#include "motion.h"

#include <stdio.h>
#include <string.h>

static void complete(struct motion *m, int err, const char *msg)
{
	motion_done_cb done = m->done;

	m->active = false;
	ev_timer_cancel(&m->poll_timer);
	if (err != ESP_OK)
		snprintf(m->error, sizeof(m->error), "%s", msg);
	if (done)
		done(m->ctx, err, msg);
}

static void fail_link(struct motion *m, int err)
{
	char msg[160];

	if (m->link->last_error[0])
		snprintf(msg, sizeof(msg), "%s", m->link->last_error);
	else
		snprintf(msg, sizeof(msg), "%s", esp_err_name(err));
	complete(m, err, msg);
}

static bool current(struct motion *m)
{
	return m->active && m->req_gen == m->gen;
}

static void on_status(void *ctx, int err, const struct fj_object *resp)
{
	struct motion *m = ctx;
	const struct esp_status *st = &m->link->status;

	(void)resp;
	if (!current(m))
		return;
	if (err != ESP_OK) {
		fail_link(m, err);
		return;
	}
	if (!st->moving) {
		if (!st->output_enabled)
			complete(m, MOTION_E_STOPPED, "move was stopped before completion");
		else
			complete(m, ESP_OK, NULL);
		return;
	}
	if (ev_now() >= m->deadline) {
		complete(m, MOTION_E_TIMEOUT, "movement did not finish within the timeout");
		return;
	}
	ev_timer_set(&m->poll_timer, MOTION_POLL_MS);
}

static void on_poll(struct ev_timer *t)
{
	struct motion *m = ev_container_of(t, struct motion, poll_timer);
	int err;

	if (!m->active)
		return;
	m->req_gen = m->gen;
	err = esp_link_get_status(m->link, on_status, m);
	if (err == ESP_E_BUSY) {
		ev_timer_set(&m->poll_timer, MOTION_POLL_MS); /* e.g. a stop is queued */
		return;
	}
	if (err != ESP_OK)
		fail_link(m, err);
}

static void on_set(void *ctx, int err, const struct fj_object *resp)
{
	struct motion *m = ctx;
	int move_ms = 0;
	bool moving = true;

	if (!current(m))
		return;
	if (err != ESP_OK) {
		fail_link(m, err);
		return;
	}

	fj_get_bool(resp, "moving", &moving);
	fj_get_int(resp, "move_time_ms", &move_ms);
	if (!moving) {
		complete(m, ESP_OK, NULL);
		return;
	}

	m->deadline = ev_now() + m->timeout_ms;
	if (move_ms < MOTION_POLL_MS)
		move_ms = MOTION_POLL_MS;
	if (move_ms > m->timeout_ms)
		move_ms = m->timeout_ms;
	ev_timer_set(&m->poll_timer, move_ms);
}

void motion_init(struct motion *m, struct esp_link *link)
{
	memset(m, 0, sizeof(*m));
	m->link = link;
	m->poll_timer.cb = on_poll;
}

int motion_move(struct motion *m, int angle, int timeout_ms, motion_done_cb done, void *ctx)
{
	int err;

	if (m->active)
		return ESP_E_BUSY;

	m->active = true;
	m->target = angle;
	m->timeout_ms = timeout_ms;
	m->done = done;
	m->ctx = ctx;
	m->error[0] = '\0';
	m->req_gen = m->gen;
	err = esp_link_set_position(m->link, angle, on_set, m);
	if (err != ESP_OK)
		m->active = false;
	return err;
}

void motion_abort(struct motion *m)
{
	m->active = false;
	m->gen++;
	ev_timer_cancel(&m->poll_timer);
}
