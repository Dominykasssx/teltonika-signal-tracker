#include "signal.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Reporting ranges from 3GPP TS 36.133 (including extended RSRQ); anything else is invalid. */
#define RSRP_MIN -156.0
#define RSRP_MAX -31.0
#define RSRQ_MIN -34.0
#define RSRQ_MAX 3.0
#define SINR_MIN -23.0
#define SINR_MAX 40.0
#define RSSI_MIN -120.0
#define RSSI_MAX -10.0

void signal_sample_clear(struct signal_sample *s)
{
	memset(s, 0, sizeof(*s));
}

static bool parse_value(const char *p, double min, double max, double *out)
{
	char *end;
	double v;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\0')
		return false;
	v = strtod(p, &end);
	if (end == p || !isfinite(v))
		return false;
	while (*end == ' ' || *end == '\t' || *end == '\r')
		end++;
	/* allow a unit suffix such as "dBm" or "dB" */
	while (isalpha((unsigned char)*end))
		end++;
	if (*end != '\0' && *end != '\n')
		return false;
	if (v < min || v > max)
		return false;
	*out = v;
	return true;
}

int signal_parse_levels(const char *text, struct signal_sample *s)
{
	const char *line = text;
	int found = 0;

	while (line && *line) {
		const char *nl = strchr(line, '\n');
		size_t len = nl ? (size_t)(nl - line) : strlen(line);
		char buf[128];
		char *colon;
		double v;

		if (len >= sizeof(buf))
			len = sizeof(buf) - 1;
		memcpy(buf, line, len);
		buf[len] = '\0';
		line = nl ? nl + 1 : NULL;

		colon = strchr(buf, ':');
		if (!colon)
			continue;
		*colon = '\0';

		if (strcasecmp(buf, "RSRP") == 0 && parse_value(colon + 1, RSRP_MIN, RSRP_MAX, &v)) {
			s->has_rsrp = true;
			s->rsrp = v;
			found++;
		} else if (strcasecmp(buf, "RSRQ") == 0 &&
			   parse_value(colon + 1, RSRQ_MIN, RSRQ_MAX, &v)) {
			s->has_rsrq = true;
			s->rsrq = v;
			found++;
		} else if (strcasecmp(buf, "SINR") == 0 &&
			   parse_value(colon + 1, SINR_MIN, SINR_MAX, &v)) {
			s->has_sinr = true;
			s->sinr = v;
			found++;
		} else if (strcasecmp(buf, "RSSI") == 0 &&
			   parse_value(colon + 1, RSSI_MIN, RSSI_MAX, &v)) {
			s->has_rssi = true;
			s->rssi = v;
			found++;
		}
	}
	return found;
}

bool signal_parse_cell(const char *text, struct signal_sample *s)
{
	size_t n = 0;

	while (*text == ' ' || *text == '\t')
		text++;
	while (text[n] && text[n] != '\n' && text[n] != '\r' && text[n] != ' ') {
		if (!isxdigit((unsigned char)text[n]) || n + 1 >= sizeof(s->cell_id))
			return false;
		n++;
	}
	if (n == 0)
		return false;
	memcpy(s->cell_id, text, n);
	s->cell_id[n] = '\0';
	s->has_cell = true;
	return true;
}

bool signal_has_metrics(const struct signal_sample *s)
{
	return s->ok && (s->has_rsrp || s->has_rsrq || s->has_sinr);
}
