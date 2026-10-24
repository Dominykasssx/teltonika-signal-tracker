/*
 * Asynchronous antenna scan.
 *
 * Optionally measures the starting angle (baseline), sweeps a coarse grid, refines with
 * a fine grid around the best coarse angle and moves back to the best measured angle.
 * The scan never blocks: moves and samples are requested through scan_ops and their
 * results are reported back with scan_move_done() / scan_sample_done(). Each request
 * carries a token so results arriving after a cancel or failure are ignored.
 */
#ifndef SCAN_H
#define SCAN_H

#include <stdbool.h>

#include "ev.h"
#include "score.h"
#include "signal.h"

#define SCAN_MAX_ANGLES 400
#define SCAN_MAX_SAMPLES 20
#define SCAN_MAX_UNSCORED 3 /* consecutive angles without valid data before giving up */

enum scan_state {
	SCAN_IDLE,
	SCAN_RUNNING,
	SCAN_DONE,
	SCAN_CANCELLED,
	SCAN_FAILED,
};

enum scan_phase {
	SCAN_PHASE_NONE,
	SCAN_PHASE_BASELINE,
	SCAN_PHASE_COARSE,
	SCAN_PHASE_FINE,
	SCAN_PHASE_RETURN,
};

struct scan_params {
	int min_angle;
	int max_angle;
	int coarse_step;
	int fine_step;
	int settle_ms;
	int samples;
	int sample_interval_ms;
	int min_valid;
	int move_timeout_ms;
	double min_improvement;
	struct score_weights weights;
};

struct scan;

struct scan_ops {
	/* Start moving; return 0 if started. Report with scan_move_done(). */
	int (*move)(struct scan *s, unsigned token, int angle, int timeout_ms);
	/* Take one fresh signal sample; return 0 if started. Report with scan_sample_done(). */
	int (*sample)(struct scan *s, unsigned token);
	/* Optional: state, phase or progress changed. */
	void (*changed)(struct scan *s);
};

enum scan_wait {
	SCAN_WAIT_NONE,
	SCAN_WAIT_MOVE,
	SCAN_WAIT_SETTLE,
	SCAN_WAIT_SAMPLE,
};

struct scan {
	const struct scan_ops *ops;
	void *ctx;
	struct scan_params p;

	enum scan_state state;
	enum scan_phase phase;
	enum scan_wait wait;
	unsigned token;
	struct ev_timer timer;

	int plan[SCAN_MAX_ANGLES];
	int plan_len;
	int plan_pos;

	struct angle_result results[SCAN_MAX_ANGLES];
	int n_results;
	int baseline_idx; /* -1 when the start position was unknown */
	int best_idx;     /* -1 when nothing scored */

	bool position_known;
	int position;      /* where the scan last put the servo */
	int current_angle; /* angle being measured or moved to */
	struct signal_sample samples[SCAN_MAX_SAMPLES];
	int n_samples;
	int consecutive_unscored;

	int total_steps;
	int done_steps;
	bool complete;      /* every planned angle measured and return move done */
	bool cell_changes;  /* serving cell differed between angles */
	bool kept_baseline; /* improvement below threshold, returned to start angle */
	int final_angle;
	bool has_final;
	char message[160];
};

void scan_init(struct scan *s, const struct scan_ops *ops, void *ctx);
/* Returns 0 if started, -1 if a scan is already running. */
int scan_start(struct scan *s, const struct scan_params *p, bool start_known, int start_angle);
void scan_cancel(struct scan *s, const char *reason);
void scan_fail(struct scan *s, const char *reason);

void scan_move_done(struct scan *s, unsigned token, int err, const char *msg);
void scan_sample_done(struct scan *s, unsigned token, const struct signal_sample *sample);

bool scan_running(const struct scan *s);
int scan_progress(const struct scan *s);
const char *scan_state_name(enum scan_state st);
const char *scan_phase_name(enum scan_phase ph);

#endif
