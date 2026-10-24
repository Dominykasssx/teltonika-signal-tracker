/*
 * Signal scoring for one antenna angle.
 *
 * Each metric is reduced to the median of the valid samples, mapped linearly onto 0..1
 * over a practical range, and combined as a weighted mean over the metrics that are
 * actually available (weights are renormalised; missing metrics are never zero).
 * Samples from a minority serving cell are discarded so a handover mid-measurement
 * does not mix two cells into one value.
 */
#ifndef SCORE_H
#define SCORE_H

#include <stdbool.h>

#include "signal.h"

/* Normalisation ranges (value at 0 and at 100 points). */
#define SCORE_RSRP_LOW -140.0
#define SCORE_RSRP_HIGH -44.0
#define SCORE_RSRQ_LOW -20.0
#define SCORE_RSRQ_HIGH -3.0
#define SCORE_SINR_LOW -10.0
#define SCORE_SINR_HIGH 30.0

struct score_weights {
	double rsrp;
	double rsrq;
	double sinr;
};

struct angle_result {
	int angle;
	int samples; /* samples taken */
	int valid;   /* samples used after validity and serving-cell filtering */
	bool scored;
	double score; /* 0..100, meaningful only when scored */
	bool has_rsrp, has_rsrq, has_sinr;
	double rsrp, rsrq, sinr; /* medians */
	bool cell_changed;       /* more than one serving cell seen at this angle */
	bool has_cell;
	char cell_id[SIGNAL_CELL_MAX];
};

/* Sorts v in place. n must be > 0. */
double score_median(double *v, int n);

/*
 * A metric is used only if at least min_valid samples report it, and the angle is
 * scored only if at least min_valid samples survive filtering and one weighted metric
 * is available.
 */
void score_angle(int angle, const struct signal_sample *samples, int n, int min_valid,
		 const struct score_weights *w, struct angle_result *out);

/* Index of the best scored result, or -1 if none is scored. Ties: higher RSRP, then first. */
int score_best(const struct angle_result *results, int n);

#endif
