#include "score.h"

#include <stdlib.h>
#include <string.h>

#define SCORE_MAX_SAMPLES 64
#define SCORE_EPSILON 1e-9

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

double score_median(double *v, int n)
{
	qsort(v, (size_t)n, sizeof(*v), cmp_double);
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static double normalise(double v, double low, double high)
{
	double x = (v - low) / (high - low);

	return x < 0 ? 0 : (x > 1 ? 1 : x);
}

/* Most frequent cell id among usable samples; empty string if none report a cell. */
static void majority_cell(const struct signal_sample *s, int n, char *out, bool *changed)
{
	int best_count = 0;

	out[0] = '\0';
	*changed = false;
	for (int i = 0; i < n; i++) {
		int count = 0;

		if (!signal_has_metrics(&s[i]) || !s[i].has_cell)
			continue;
		for (int j = 0; j < n; j++) {
			if (signal_has_metrics(&s[j]) && s[j].has_cell &&
			    strcmp(s[i].cell_id, s[j].cell_id) == 0)
				count++;
		}
		if (count > best_count) {
			best_count = count;
			strcpy(out, s[i].cell_id);
		}
	}
	for (int i = 0; i < n; i++) {
		if (signal_has_metrics(&s[i]) && s[i].has_cell && strcmp(out, s[i].cell_id) != 0)
			*changed = true;
	}
}

void score_angle(int angle, const struct signal_sample *s, int n, int min_valid,
		 const struct score_weights *w, struct angle_result *out)
{
	double rsrp[SCORE_MAX_SAMPLES], rsrq[SCORE_MAX_SAMPLES], sinr[SCORE_MAX_SAMPLES];
	int n_rsrp = 0, n_rsrq = 0, n_sinr = 0;
	char cell[SIGNAL_CELL_MAX];
	double sum = 0, wsum = 0;

	if (n > SCORE_MAX_SAMPLES)
		n = SCORE_MAX_SAMPLES;
	if (min_valid < 1)
		min_valid = 1;

	memset(out, 0, sizeof(*out));
	out->angle = angle;
	out->samples = n;

	majority_cell(s, n, cell, &out->cell_changed);
	if (cell[0]) {
		out->has_cell = true;
		strcpy(out->cell_id, cell);
	}

	for (int i = 0; i < n; i++) {
		if (!signal_has_metrics(&s[i]))
			continue;
		/* samples without a cell id are kept; samples from another cell are not */
		if (cell[0] && s[i].has_cell && strcmp(cell, s[i].cell_id) != 0)
			continue;
		out->valid++;
		if (s[i].has_rsrp)
			rsrp[n_rsrp++] = s[i].rsrp;
		if (s[i].has_rsrq)
			rsrq[n_rsrq++] = s[i].rsrq;
		if (s[i].has_sinr)
			sinr[n_sinr++] = s[i].sinr;
	}

	if (n_rsrp >= min_valid) {
		out->has_rsrp = true;
		out->rsrp = score_median(rsrp, n_rsrp);
	}
	if (n_rsrq >= min_valid) {
		out->has_rsrq = true;
		out->rsrq = score_median(rsrq, n_rsrq);
	}
	if (n_sinr >= min_valid) {
		out->has_sinr = true;
		out->sinr = score_median(sinr, n_sinr);
	}

	if (out->valid < min_valid)
		return;

	if (out->has_rsrp && w->rsrp > 0) {
		sum += w->rsrp * normalise(out->rsrp, SCORE_RSRP_LOW, SCORE_RSRP_HIGH);
		wsum += w->rsrp;
	}
	if (out->has_rsrq && w->rsrq > 0) {
		sum += w->rsrq * normalise(out->rsrq, SCORE_RSRQ_LOW, SCORE_RSRQ_HIGH);
		wsum += w->rsrq;
	}
	if (out->has_sinr && w->sinr > 0) {
		sum += w->sinr * normalise(out->sinr, SCORE_SINR_LOW, SCORE_SINR_HIGH);
		wsum += w->sinr;
	}
	if (wsum <= 0)
		return;

	out->scored = true;
	out->score = 100.0 * sum / wsum;
}

int score_best(const struct angle_result *r, int n)
{
	int best = -1;

	for (int i = 0; i < n; i++) {
		if (!r[i].scored)
			continue;
		if (best < 0 || r[i].score > r[best].score + SCORE_EPSILON) {
			best = i;
		} else if (r[i].score > r[best].score - SCORE_EPSILON && r[i].has_rsrp &&
			   (!r[best].has_rsrp || r[i].rsrp > r[best].rsrp)) {
			best = i;
		}
	}
	return best;
}
