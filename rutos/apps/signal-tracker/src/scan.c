#include "scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void measure_next(struct scan *s);

static void changed(struct scan *s)
{
	if (s->ops->changed)
		s->ops->changed(s);
}

static void stop(struct scan *s, enum scan_state state, const char *message)
{
	s->state = state;
	s->wait = SCAN_WAIT_NONE;
	s->token++; /* results of requests still in flight are ignored */
	ev_timer_cancel(&s->timer);
	s->best_idx = score_best(s->results, s->n_results);
	snprintf(s->message, sizeof(s->message), "%s", message);
	changed(s);
}

static void fail(struct scan *s, const char *message)
{
	stop(s, SCAN_FAILED, message);
}

static bool measured(const struct scan *s, int angle)
{
	for (int i = 0; i < s->n_results; i++) {
		if (s->results[i].angle == angle)
			return true;
	}
	return false;
}

static void plan_add(struct scan *s, int angle)
{
	if (s->plan_len < SCAN_MAX_ANGLES && !measured(s, angle))
		s->plan[s->plan_len++] = angle;
}

/* Plans are built in ascending order; sweep from whichever end is closer to the servo. */
static void plan_order(struct scan *s)
{
	if (s->plan_len < 2 || !s->position_known)
		return;
	if (abs(s->position - s->plan[s->plan_len - 1]) < abs(s->position - s->plan[0])) {
		for (int i = 0, j = s->plan_len - 1; i < j; i++, j--) {
			int t = s->plan[i];

			s->plan[i] = s->plan[j];
			s->plan[j] = t;
		}
	}
}

static void begin_phase(struct scan *s, enum scan_phase phase)
{
	s->phase = phase;
	s->plan_len = 0;
	s->plan_pos = 0;
}

static int coarse_count(const struct scan_params *p)
{
	int span = p->max_angle - p->min_angle;

	return span / p->coarse_step + 1 + (span % p->coarse_step ? 1 : 0);
}

static int fine_estimate(const struct scan_params *p)
{
	return 2 * ((p->coarse_step - p->fine_step) / p->fine_step);
}

static void start_return(struct scan *s)
{
	const struct angle_result *best;

	s->best_idx = score_best(s->results, s->n_results);
	if (s->best_idx < 0) {
		fail(s, "No angle produced valid signal measurements");
		return;
	}
	best = &s->results[s->best_idx];
	s->final_angle = best->angle;
	s->kept_baseline = false;

	if (s->baseline_idx >= 0 && s->p.min_improvement > 0 &&
	    s->results[s->baseline_idx].scored &&
	    best->score - s->results[s->baseline_idx].score < s->p.min_improvement) {
		s->final_angle = s->results[s->baseline_idx].angle;
		s->kept_baseline = true;
	}
	s->has_final = true;

	begin_phase(s, SCAN_PHASE_RETURN);
	s->total_steps = s->done_steps + 1;
	s->current_angle = s->final_angle;
	s->wait = SCAN_WAIT_MOVE;
	if (s->position_known && s->position == s->final_angle) {
		scan_move_done(s, s->token, 0, NULL); /* already there */
		return;
	}
	changed(s);
	if (s->ops->move(s, s->token, s->final_angle, s->p.move_timeout_ms) != 0)
		fail(s, "Could not send the final move command");
}

static void start_fine(struct scan *s)
{
	int c, reach = s->p.coarse_step - s->p.fine_step;

	s->best_idx = score_best(s->results, s->n_results);
	if (s->best_idx < 0) {
		fail(s, "No angle produced valid signal measurements");
		return;
	}
	c = s->results[s->best_idx].angle;

	begin_phase(s, SCAN_PHASE_FINE);
	for (int d = -reach; d <= reach; d += s->p.fine_step) {
		int a = c + d;

		if (a >= s->p.min_angle && a <= s->p.max_angle)
			plan_add(s, a);
	}
	plan_order(s);
	s->total_steps = s->done_steps + s->plan_len + 1;

	if (s->plan_len == 0)
		start_return(s);
	else
		measure_next(s);
}

static void start_coarse(struct scan *s)
{
	int a;

	begin_phase(s, SCAN_PHASE_COARSE);
	for (a = s->p.min_angle; a <= s->p.max_angle; a += s->p.coarse_step)
		plan_add(s, a);
	if (a - s->p.coarse_step != s->p.max_angle)
		plan_add(s, s->p.max_angle);
	plan_order(s);

	if (s->plan_len == 0)
		start_fine(s);
	else
		measure_next(s);
}

static void next_phase(struct scan *s)
{
	switch (s->phase) {
	case SCAN_PHASE_BASELINE:
		start_coarse(s);
		break;
	case SCAN_PHASE_COARSE:
		start_fine(s);
		break;
	case SCAN_PHASE_FINE:
		start_return(s);
		break;
	default:
		break;
	}
}

static void take_sample(struct scan *s)
{
	s->wait = SCAN_WAIT_SAMPLE;
	if (s->ops->sample(s, s->token) != 0) {
		struct signal_sample bad;

		signal_sample_clear(&bad);
		snprintf(bad.error, sizeof(bad.error), "measurement could not be started");
		scan_sample_done(s, s->token, &bad);
	}
}

static void on_timer(struct ev_timer *t)
{
	struct scan *s = ev_container_of(t, struct scan, timer);

	if (s->state == SCAN_RUNNING && s->wait == SCAN_WAIT_SETTLE)
		take_sample(s);
}

/* Measures plan[plan_pos]; the baseline phase measures in place without moving. */
static void measure_next(struct scan *s)
{
	if (s->plan_pos >= s->plan_len) {
		next_phase(s);
		return;
	}

	s->current_angle = s->plan[s->plan_pos];
	s->n_samples = 0;
	changed(s);

	if (s->phase == SCAN_PHASE_BASELINE) {
		s->wait = SCAN_WAIT_SETTLE;
		ev_timer_set(&s->timer, 0);
		return;
	}

	s->wait = SCAN_WAIT_MOVE;
	if (s->ops->move(s, s->token, s->current_angle, s->p.move_timeout_ms) != 0)
		fail(s, "Could not send a move command");
}

static void finish_angle(struct scan *s)
{
	struct angle_result *r;
	char msg[160];

	if (s->n_results >= SCAN_MAX_ANGLES) {
		fail(s, "Too many scan angles");
		return;
	}
	r = &s->results[s->n_results];
	score_angle(s->current_angle, s->samples, s->n_samples, s->p.min_valid, &s->p.weights, r);
	if (s->phase == SCAN_PHASE_BASELINE)
		s->baseline_idx = s->n_results;
	s->n_results++;
	s->done_steps++;

	for (int i = 0; i < s->n_results - 1; i++) {
		if (r->has_cell && s->results[i].has_cell &&
		    strcmp(r->cell_id, s->results[i].cell_id) != 0)
			s->cell_changes = true;
	}

	if (r->scored) {
		s->consecutive_unscored = 0;
	} else if (++s->consecutive_unscored >= SCAN_MAX_UNSCORED) {
		snprintf(msg, sizeof(msg),
			 "No valid signal measurements at %d consecutive angles (last: %d)",
			 SCAN_MAX_UNSCORED, r->angle);
		fail(s, msg);
		return;
	}
	s->best_idx = score_best(s->results, s->n_results);

	s->plan_pos++;
	measure_next(s);
}

void scan_init(struct scan *s, const struct scan_ops *ops, void *ctx)
{
	memset(s, 0, sizeof(*s));
	s->ops = ops;
	s->ctx = ctx;
	s->timer.cb = on_timer;
	s->baseline_idx = -1;
	s->best_idx = -1;
}

int scan_start(struct scan *s, const struct scan_params *p, bool start_known, int start_angle)
{
	if (s->state == SCAN_RUNNING)
		return -1;

	ev_timer_cancel(&s->timer);
	s->p = *p;
	if (s->p.samples > SCAN_MAX_SAMPLES)
		s->p.samples = SCAN_MAX_SAMPLES;
	s->state = SCAN_RUNNING;
	s->phase = SCAN_PHASE_NONE;
	s->wait = SCAN_WAIT_NONE;
	s->token++;
	s->n_results = 0;
	s->baseline_idx = -1;
	s->best_idx = -1;
	s->consecutive_unscored = 0;
	s->done_steps = 0;
	s->complete = false;
	s->cell_changes = false;
	s->kept_baseline = false;
	s->has_final = false;
	s->message[0] = '\0';

	s->position_known = start_known && start_angle >= p->min_angle && start_angle <= p->max_angle;
	s->position = start_angle;
	s->total_steps = (s->position_known ? 1 : 0) + coarse_count(p) + fine_estimate(p) + 1;

	if (s->position_known) {
		begin_phase(s, SCAN_PHASE_BASELINE);
		s->plan[s->plan_len++] = start_angle;
		measure_next(s);
	} else {
		start_coarse(s);
	}
	return 0;
}

void scan_cancel(struct scan *s, const char *reason)
{
	if (s->state == SCAN_RUNNING)
		stop(s, SCAN_CANCELLED, reason);
}

void scan_fail(struct scan *s, const char *reason)
{
	if (s->state == SCAN_RUNNING)
		fail(s, reason);
}

void scan_move_done(struct scan *s, unsigned token, int err, const char *msg)
{
	char buf[160];

	if (token != s->token || s->state != SCAN_RUNNING || s->wait != SCAN_WAIT_MOVE)
		return;

	if (err) {
		s->position_known = false;
		snprintf(buf, sizeof(buf), "Move to %d failed: %s", s->current_angle,
			 msg ? msg : "unknown error");
		fail(s, buf);
		return;
	}
	s->position_known = true;
	s->position = s->current_angle;

	if (s->phase == SCAN_PHASE_RETURN) {
		s->done_steps++;
		s->complete = true;
		if (s->kept_baseline)
			snprintf(buf, sizeof(buf),
				 "Scan complete. Improvement below threshold, kept starting angle %d",
				 s->final_angle);
		else
			snprintf(buf, sizeof(buf), "Scan complete. Moved to best measured angle %d",
				 s->final_angle);
		stop(s, SCAN_DONE, buf);
		return;
	}

	s->wait = SCAN_WAIT_SETTLE;
	ev_timer_set(&s->timer, s->p.settle_ms);
}

void scan_sample_done(struct scan *s, unsigned token, const struct signal_sample *sample)
{
	if (token != s->token || s->state != SCAN_RUNNING || s->wait != SCAN_WAIT_SAMPLE)
		return;

	if (s->n_samples < SCAN_MAX_SAMPLES)
		s->samples[s->n_samples++] = *sample;
	changed(s);

	if (s->n_samples < s->p.samples) {
		s->wait = SCAN_WAIT_SETTLE;
		ev_timer_set(&s->timer, s->p.sample_interval_ms);
		return;
	}
	s->wait = SCAN_WAIT_NONE;
	finish_angle(s);
}

bool scan_running(const struct scan *s)
{
	return s->state == SCAN_RUNNING;
}

int scan_progress(const struct scan *s)
{
	int pct;

	if (s->state == SCAN_DONE)
		return 100;
	if (s->total_steps <= 0)
		return 0;
	pct = s->done_steps * 100 / s->total_steps;
	return pct > 99 ? 99 : pct;
}

const char *scan_state_name(enum scan_state st)
{
	switch (st) {
	case SCAN_IDLE: return "idle";
	case SCAN_RUNNING: return "running";
	case SCAN_DONE: return "done";
	case SCAN_CANCELLED: return "cancelled";
	case SCAN_FAILED: return "failed";
	}
	return "unknown";
}

const char *scan_phase_name(enum scan_phase ph)
{
	switch (ph) {
	case SCAN_PHASE_NONE: return "none";
	case SCAN_PHASE_BASELINE: return "baseline";
	case SCAN_PHASE_COARSE: return "coarse";
	case SCAN_PHASE_FINE: return "fine";
	case SCAN_PHASE_RETURN: return "returning";
	}
	return "unknown";
}
