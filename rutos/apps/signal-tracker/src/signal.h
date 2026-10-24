/*
 * Cellular signal samples and parsing of RUTOS gsmctl output.
 *
 * gsmctl -q prints one "NAME: value" line per metric (RSSI, RSRP, SINR, RSRQ on LTE);
 * gsmctl -C prints the serving cell id. Values outside physically plausible ranges are
 * treated as missing, never as zero.
 */
#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdbool.h>

#define SIGNAL_CELL_MAX 24

struct signal_sample {
	bool ok; /* the measurement command ran and produced output */
	bool has_rsrp, has_rsrq, has_sinr, has_rssi;
	double rsrp, rsrq, sinr, rssi;
	bool has_cell;
	char cell_id[SIGNAL_CELL_MAX];
	long long time_ms;
	char error[96];
};

void signal_sample_clear(struct signal_sample *s);
/* Parses gsmctl -q output; returns the number of valid metrics found. */
int signal_parse_levels(const char *text, struct signal_sample *s);
/* Parses gsmctl -C output; returns true if a plausible cell id was found. */
bool signal_parse_cell(const char *text, struct signal_sample *s);
bool signal_has_metrics(const struct signal_sample *s);

#endif
