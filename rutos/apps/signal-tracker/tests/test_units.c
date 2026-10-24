/* Unit tests: line framing, gsmctl parsing, scoring, config validation, link protocol. */
#include <math.h>
#include <string.h>

#include "config.h"
#include "esp_link.h"
#include "harness.h"
#include "linebuf.h"
#include "score.h"
#include "signal.h"

/* ------------------------------------------------------------------ linebuf */

static char lines[8][LINEBUF_MAX];
static int n_lines, n_dropped;

static void collect(void *ctx, const char *line)
{
	(void)ctx;
	if (!line) {
		n_dropped++;
		return;
	}
	if (n_lines < 8)
		strcpy(lines[n_lines++], line);
}

static void test_linebuf(void)
{
	struct linebuf lb;
	char big[LINEBUF_MAX + 50];

	linebuf_reset(&lb);
	n_lines = n_dropped = 0;

	/* partial reads */
	linebuf_feed(&lb, "{\"a\":", 5, collect, NULL);
	CHECK(n_lines == 0);
	linebuf_feed(&lb, "1}\r\n", 4, collect, NULL);
	CHECK(n_lines == 1 && strcmp(lines[0], "{\"a\":1}") == 0);

	/* several messages in one read, empty lines skipped */
	linebuf_feed(&lb, "one\n\ntwo\nthr", 12, collect, NULL);
	CHECK(n_lines == 3 && strcmp(lines[1], "one") == 0 && strcmp(lines[2], "two") == 0);
	linebuf_feed(&lb, "ee\n", 3, collect, NULL);
	CHECK(n_lines == 4 && strcmp(lines[3], "three") == 0);

	/* over-long line dropped, stream recovers at the next newline */
	memset(big, 'x', sizeof(big));
	linebuf_feed(&lb, big, sizeof(big), collect, NULL);
	linebuf_feed(&lb, "\nok\n", 4, collect, NULL);
	CHECK(n_dropped == 1);
	CHECK(n_lines == 5 && strcmp(lines[4], "ok") == 0);
}

/* ------------------------------------------------------------------ signal parsing */

static void test_signal_parse(void)
{
	struct signal_sample s;

	signal_sample_clear(&s);
	CHECK(signal_parse_levels("RSSI: -60\nRSRP: -103\nSINR: 2\nRSRQ: -16\n", &s) == 4);
	CHECK(s.has_rsrp && s.rsrp == -103 && s.has_rsrq && s.rsrq == -16);
	CHECK(s.has_sinr && s.sinr == 2 && s.has_rssi && s.rssi == -60);

	/* missing and unavailable metrics are absent, never zero */
	signal_sample_clear(&s);
	CHECK(signal_parse_levels("RSRP: N/A\nRSRQ: -12 dB\n", &s) == 1);
	CHECK(!s.has_rsrp && s.has_rsrq && s.rsrq == -12 && !s.has_sinr);

	/* implausible values (sentinels) are rejected */
	signal_sample_clear(&s);
	CHECK(signal_parse_levels("RSRP: 0\nSINR: 255\nRSRQ: -99\nRSSI: 1\n", &s) == 0);
	CHECK(!s.has_rsrp && !s.has_sinr && !s.has_rsrq && !s.has_rssi);

	signal_sample_clear(&s);
	CHECK(signal_parse_levels("Error: modem not found\n", &s) == 0);
	CHECK(signal_parse_levels("", &s) == 0);
	CHECK(signal_parse_levels("RSRP: -90abc1\n", &s) == 0);

	signal_sample_clear(&s);
	CHECK(signal_parse_cell("1037089\n", &s) && strcmp(s.cell_id, "1037089") == 0);
	signal_sample_clear(&s);
	CHECK(signal_parse_cell("  1A2b3C", &s) && strcmp(s.cell_id, "1A2b3C") == 0);
	signal_sample_clear(&s);
	CHECK(!signal_parse_cell("ERROR", &s) && !s.has_cell);
	CHECK(!signal_parse_cell("", &s));
	CHECK(!signal_parse_cell("123456789012345678901234567890", &s));

	signal_sample_clear(&s);
	s.has_rsrp = true;
	CHECK(!signal_has_metrics(&s)); /* ok flag not set: command failed */
	s.ok = true;
	CHECK(signal_has_metrics(&s));
}

/* ------------------------------------------------------------------ scoring */

static struct signal_sample mk(double rsrp, double rsrq, double sinr, const char *cell)
{
	struct signal_sample s;

	signal_sample_clear(&s);
	s.ok = true;
	if (!isnan(rsrp)) {
		s.has_rsrp = true;
		s.rsrp = rsrp;
	}
	if (!isnan(rsrq)) {
		s.has_rsrq = true;
		s.rsrq = rsrq;
	}
	if (!isnan(sinr)) {
		s.has_sinr = true;
		s.sinr = sinr;
	}
	if (cell) {
		s.has_cell = true;
		strcpy(s.cell_id, cell);
	}
	return s;
}

static void test_score(void)
{
	const struct score_weights w = { .rsrp = 0.5, .rsrq = 0.2, .sinr = 0.3 };
	struct signal_sample s[6];
	struct angle_result r[4];
	double v[] = { 3, 1, 2, 10 };

	CHECK(score_median(v, 3) == 2);
	CHECK(score_median(v, 4) == 2.5);

	/* median rejects a single outlier */
	s[0] = mk(-90, -10, 10, "A");
	s[1] = mk(-91, -10, 10, "A");
	s[2] = mk(-60, -10, 10, "A");
	s[3] = mk(-89, -10, 10, "A");
	s[4] = mk(-90, -10, 10, "A");
	score_angle(30, s, 5, 3, &w, &r[0]);
	CHECK(r[0].scored && r[0].rsrp == -90 && r[0].valid == 5 && !r[0].cell_changed);
	CHECK(r[0].score > 0 && r[0].score < 100);

	/* a missing metric is excluded and the weights renormalised, not scored as zero */
	s[0] = mk(-90, NAN, NAN, NULL);
	s[1] = mk(-90, NAN, NAN, NULL);
	score_angle(10, s, 2, 2, &w, &r[1]);
	s[0] = mk(-90, NAN, 0, NULL);
	s[1] = mk(-90, NAN, 0, NULL);
	score_angle(20, s, 2, 2, &w, &r[2]);
	CHECK(r[1].scored && !r[1].has_sinr && !r[1].has_rsrq);
	CHECK(fabs(r[1].score - 100.0 * (-90 - SCORE_RSRP_LOW) / (SCORE_RSRP_HIGH - SCORE_RSRP_LOW)) < 1e-6);
	CHECK(r[2].scored && r[2].has_sinr);
	CHECK(r[1].score > r[2].score); /* SINR 0 dB is poor; an unknown SINR is not */

	/* too few valid samples: not scored */
	s[0] = mk(-80, -8, 15, NULL);
	s[1] = mk(-80, -8, 15, NULL);
	signal_sample_clear(&s[2]); /* failed measurement */
	s[3] = mk(NAN, NAN, NAN, NULL); /* ran, but no metrics */
	score_angle(40, s, 4, 3, &w, &r[3]);
	CHECK(!r[3].scored && r[3].valid == 2 && r[3].samples == 4);

	/* metric reported by too few samples is dropped, others still score */
	s[0] = mk(-80, -8, 15, NULL);
	s[1] = mk(-80, -8, NAN, NULL);
	s[2] = mk(-80, -8, NAN, NULL);
	score_angle(50, s, 3, 2, &w, &r[3]);
	CHECK(r[3].scored && !r[3].has_sinr && r[3].has_rsrq);

	/* serving cell change: minority cell samples are discarded */
	s[0] = mk(-100, -12, 5, "A");
	s[1] = mk(-70, -6, 20, "B");
	s[2] = mk(-100, -12, 5, "A");
	s[3] = mk(-70, -6, 20, "B");
	s[4] = mk(-100, -12, 5, "A");
	score_angle(60, s, 5, 3, &w, &r[0]);
	CHECK(r[0].cell_changed && strcmp(r[0].cell_id, "A") == 0);
	CHECK(r[0].valid == 3 && r[0].rsrp == -100);

	/* nothing usable at all */
	signal_sample_clear(&s[0]);
	score_angle(70, s, 1, 1, &w, &r[0]);
	CHECK(!r[0].scored && r[0].valid == 0);

	/* best selection: unscored skipped, ties broken by RSRP */
	memset(r, 0, sizeof(r));
	r[0].scored = false;
	r[1].scored = true; r[1].score = 50; r[1].has_rsrp = true; r[1].rsrp = -95;
	r[2].scored = true; r[2].score = 50; r[2].has_rsrp = true; r[2].rsrp = -90;
	r[3].scored = true; r[3].score = 49.9;
	CHECK(score_best(r, 4) == 2);
	r[1].scored = r[2].scored = r[3].scored = false;
	CHECK(score_best(r, 4) == -1);
	CHECK(score_best(r, 0) == -1);
}

/* ------------------------------------------------------------------ config */

static void test_config(void)
{
	struct st_config c;
	char err[160];

	config_defaults(&c);
	CHECK(config_validate(&c, err, sizeof(err)));
	CHECK(c.coarse_step == 10 && c.fine_step == 2);

	config_defaults(&c);
	c.min_angle = 100;
	c.max_angle = 100;
	CHECK(!config_validate(&c, err, sizeof(err)));

	config_defaults(&c);
	c.max_angle = 400;
	CHECK(!config_validate(&c, err, sizeof(err)));

	config_defaults(&c);
	c.fine_step = 12;
	CHECK(!config_validate(&c, err, sizeof(err)) && strstr(err, "Fine step"));

	config_defaults(&c);
	c.coarse_step = 0;
	CHECK(!config_validate(&c, err, sizeof(err)));

	config_defaults(&c);
	c.min_valid_samples = 6;
	CHECK(!config_validate(&c, err, sizeof(err)));

	config_defaults(&c);
	c.weights.rsrp = c.weights.rsrq = c.weights.sinr = 0;
	CHECK(!config_validate(&c, err, sizeof(err)));

	config_defaults(&c);
	c.weights.rsrq = -1;
	CHECK(!config_validate(&c, err, sizeof(err)));

	config_defaults(&c);
	strcpy(c.usb_ids[0], "10c4ea60");
	CHECK(!config_validate(&c, err, sizeof(err)));

	config_defaults(&c);
	strcpy(c.device, "ttyUSB0");
	CHECK(!config_validate(&c, err, sizeof(err)));
	strcpy(c.device, "/dev/ttyUSB3");
	CHECK(config_validate(&c, err, sizeof(err)));
}

/* ------------------------------------------------------------------ link protocol */

static char written[512];
static int write_fail, resets;

static int raw_write(void *io, const char *buf, size_t len)
{
	(void)io;
	if (write_fail)
		return -1;
	strncat(written, buf, len);
	return 0;
}

static void raw_reset(void *io)
{
	(void)io;
	resets++;
}

static const struct esp_link_ops raw_ops = { .write = raw_write, .reset = raw_reset };

static int cb_count, cb_err;
static void on_done(void *ctx, int err, const struct fj_object *resp)
{
	(void)ctx;
	(void)resp;
	cb_count++;
	cb_err = err;
}

static void feed(struct esp_link *l, const char *s)
{
	esp_link_feed(l, s, strlen(s));
}

static void test_link_protocol(void)
{
	static struct esp_link l;
	char resp[160];

	ev_cancel_all();
	written[0] = '\0';
	resets = write_fail = 0;
	esp_link_init(&l, &raw_ops, NULL, 1000);
	CHECK(esp_link_get_status(&l, on_done, NULL) == ESP_E_NOT_CONNECTED);

	esp_link_connected(&l);
	CHECK(l.state == ESP_LINK_PROBING);
	CHECK(strstr(written, "\"command\":\"get_status\"") != NULL);
	CHECK(esp_link_get_status(&l, on_done, NULL) == ESP_E_NOT_CONNECTED);

	/* boot ROM noise and garbage are ignored; probe answer split over two reads */
	feed(&l, "ets Jun  8 2016 00:22:57\r\nrst:0x1 (POWERON_RESET)\r\n{broken\n");
	CHECK(l.n_noise == 2 && l.n_malformed == 1 && l.state == ESP_LINK_PROBING);
	snprintf(resp, sizeof(resp),
		 "{\"id\":%u,\"status\":\"ok\",\"commanded_angle\":null,\"moving\":false,", l.inflight.id);
	feed(&l, resp);
	CHECK(l.state == ESP_LINK_PROBING);
	feed(&l, "\"output_enabled\":false,\"min_angle\":5,\"max_angle\":175}\n");
	CHECK(l.state == ESP_LINK_UP);
	CHECK(l.status.known && !l.status.has_angle && l.status.has_limits);
	CHECK(l.status.min_angle == 5 && l.status.max_angle == 175);

	/* request and response; second request while busy is rejected */
	cb_count = 0;
	written[0] = '\0';
	CHECK(esp_link_set_position(&l, 90, on_done, NULL) == ESP_OK);
	CHECK(strstr(written, "\"command\":\"set_position\",\"angle\":90") != NULL);
	CHECK(esp_link_set_position(&l, 95, on_done, NULL) == ESP_E_BUSY);
	CHECK(!esp_link_idle(&l));

	/* stale id ignored; correct id completes; two messages in one read */
	snprintf(resp, sizeof(resp),
		 "{\"id\":%u,\"status\":\"ok\"}\n{\"id\":%u,\"status\":\"ok\",\"commanded_angle\":90,"
		 "\"moving\":true,\"move_time_ms\":300}\n", l.inflight.id + 100, l.inflight.id);
	feed(&l, resp);
	CHECK(l.n_stale == 1 && cb_count == 1 && cb_err == ESP_OK);
	CHECK(l.status.has_angle && l.status.commanded_angle == 90 && l.status.moving);

	/* remote error */
	CHECK(esp_link_set_position(&l, 200, on_done, NULL) == ESP_OK);
	snprintf(resp, sizeof(resp),
		 "{\"id\":%u,\"status\":\"error\",\"error\":\"angle_out_of_range\",\"message\":\"bad\"}\n",
		 l.inflight.id);
	feed(&l, resp);
	CHECK(cb_count == 2 && cb_err == ESP_E_REMOTE && strstr(l.last_error, "angle_out_of_range"));

	/* id-less error (request garbled in transit) belongs to the request in flight */
	CHECK(esp_link_get_status(&l, on_done, NULL) == ESP_OK);
	feed(&l, "{\"status\":\"error\",\"error\":\"invalid_json\",\"message\":\"x\"}\n");
	CHECK(cb_count == 3 && cb_err == ESP_E_REMOTE);

	/* stop is queued behind an in-flight request instead of being refused */
	CHECK(esp_link_set_position(&l, 100, on_done, NULL) == ESP_OK);
	CHECK(esp_link_stop(&l, on_done, NULL) == ESP_OK);
	CHECK(esp_link_stop(&l, on_done, NULL) == ESP_E_BUSY);
	written[0] = '\0';
	snprintf(resp, sizeof(resp), "{\"id\":%u,\"status\":\"ok\",\"moving\":true}\n", l.inflight.id);
	feed(&l, resp);
	CHECK(cb_count == 4 && strstr(written, "\"command\":\"stop\"") != NULL);
	snprintf(resp, sizeof(resp),
		 "{\"id\":%u,\"status\":\"ok\",\"moving\":false,\"output_enabled\":false,"
		 "\"interrupted\":true}\n", l.inflight.id);
	feed(&l, resp);
	CHECK(cb_count == 5 && l.status.interrupted && !l.status.output_enabled);

	/* timeouts: first one fails the request, second consecutive one resets the port */
	CHECK(esp_link_get_status(&l, on_done, NULL) == ESP_OK);
	advance(1001);
	CHECK(cb_count == 6 && cb_err == ESP_E_TIMEOUT && l.state == ESP_LINK_UP && resets == 0);
	CHECK(esp_link_get_status(&l, on_done, NULL) == ESP_OK);
	advance(1001);
	CHECK(cb_count == 7 && cb_err == ESP_E_TIMEOUT);
	CHECK(l.state == ESP_LINK_DOWN && resets == 1 && l.n_timeouts == 2);

	/* reconnect: controller reboot event completes the probe */
	esp_link_connected(&l);
	feed(&l, "{\"event\":\"boot\",\"fw\":\"1.0.0\",\"min_angle\":0,\"max_angle\":180,"
		 "\"position_feedback\":false}\n");
	CHECK(l.state == ESP_LINK_UP && strcmp(l.status.fw, "1.0.0") == 0);
	CHECK(l.last_error[0] == '\0');

	/* reboot while a request is waiting: request fails, position forgotten */
	l.status.has_angle = true;
	CHECK(esp_link_set_position(&l, 50, on_done, NULL) == ESP_OK);
	feed(&l, "{\"event\":\"boot\",\"fw\":\"1.0.0\",\"min_angle\":0,\"max_angle\":180}\n");
	CHECK(cb_count == 8 && cb_err == ESP_E_DISCONNECTED);
	CHECK(!l.status.has_angle && strstr(l.last_error, "rebooted"));

	/* keepalive probes an idle link */
	written[0] = '\0';
	advance(5000);
	CHECK(strstr(written, "get_status") != NULL);

	/* write failure drops the link; disconnect fails the waiting request */
	esp_link_disconnected(&l, "test");
	esp_link_connected(&l);
	snprintf(resp, sizeof(resp), "{\"id\":%u,\"status\":\"ok\"}\n", l.inflight.id);
	feed(&l, resp);
	CHECK(l.state == ESP_LINK_UP);
	write_fail = 1;
	resets = 0;
	CHECK(esp_link_get_status(&l, on_done, NULL) == ESP_E_IO);
	CHECK(l.state == ESP_LINK_DOWN && resets == 1);
	write_fail = 0;

	esp_link_connected(&l);
	snprintf(resp, sizeof(resp), "{\"id\":%u,\"status\":\"ok\"}\n", l.inflight.id);
	feed(&l, resp);
	CHECK(esp_link_get_status(&l, on_done, NULL) == ESP_OK);
	esp_link_disconnected(&l, "serial device disconnected");
	CHECK(cb_count == 9 && cb_err == ESP_E_DISCONNECTED && l.state == ESP_LINK_DOWN);
	advance(10000); /* no timers left running against a dead link */
	CHECK(cb_count == 9);
}

void run_unit_tests(void)
{
	test_linebuf();
	test_signal_parse();
	test_score();
	test_config();
	test_link_protocol();
}
