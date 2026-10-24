/*
 * Protocol compatibility: drives the router-side tracker against the actual ESP32
 * firmware protocol logic (esp32/servo-controller/main/controller.c).
 */
#include <string.h>

#include "controller.h"
#include "harness.h"
#include "sim.h"
#include "tracker.h"

static struct tracker t;
static struct sim_transport tr;
static struct controller fw;
static struct sim_controller pos; /* only tracks position for the synthetic signal */
static struct sim_sampler smp;
static int servo_angle = -1;
static char msg[160];

static int fw_set(void *ctx, int angle)
{
	(void)ctx;
	servo_angle = angle;
	return 0;
}

static int fw_disable(void *ctx)
{
	(void)ctx;
	return 0;
}

static size_t fw_handle(void *dev, const char *line, long long now, char *out, size_t cap)
{
	size_t n = controller_handle_line(dev, line, now, out, cap);

	/* mirror the commanded angle into the signal model */
	pos.has_angle = fw.has_angle;
	pos.angle = fw.angle;
	pos.output = fw.output_enabled;
	pos.move_end = fw.move_end_ms;
	pos.move_start = fw.move_end_ms;
	return n;
}

static void sample_done(void *ctx, const struct signal_sample *s)
{
	tracker_sample_done(ctx, s);
}

void run_firmware_tests(void)
{
	const struct controller_config fw_cfg = {
		.min_angle = 0, .max_angle = 180, .travel_deg = 180,
		.ms_per_60deg = 250, .settle_ms = 150,
	};
	const struct servo_ops ops = { .set_angle = fw_set, .disable = fw_disable };
	struct tracker_sampler ts = { .start = sim_sampler_start, .ctx = &smp };
	struct st_config cfg;
	char boot[CONTROLLER_RESPONSE_MAX];

	ev_cancel_all();
	controller_init(&fw, &fw_cfg, &ops);
	sim_controller_init(&pos, 0, 180);
	sim_sampler_init(&smp, &pos, 96, sample_done, &t);
	smp.signal.noise = 0;
	smp.signal.width = 15;
	tracker_init(&t, &sim_transport_ops, &tr, &ts);
	sim_transport_init(&tr, &t.link, fw_handle, &fw, 5);

	config_defaults(&cfg);
	cfg.settle_ms = 300;
	cfg.samples = 3;
	cfg.min_valid_samples = 2;
	cfg.sample_interval_ms = 100;
	tracker_set_config(&t, &cfg, NULL);

	/* the firmware announces itself on boot; that completes the connect probe */
	sim_transport_connect(&tr);
	controller_boot_event(&fw, boot, sizeof(boot));
	sim_transport_inject(&tr, boot);
	advance(100);
	CHECK(t.link.state == ESP_LINK_UP);
	CHECK(strcmp(t.link.status.fw, CONTROLLER_FW_VERSION) == 0);
	CHECK(t.link.status.has_limits && !t.link.status.has_angle);

	CHECK(tracker_move(&t, 45, msg, sizeof(msg)) == NULL);
	advance(4000);
	CHECK(!t.motion.active && t.last_move_error[0] == '\0');
	CHECK(servo_angle == 45 && t.link.status.commanded_angle == 45);
	CHECK(t.link.status.output_enabled && !t.link.status.moving);

	CHECK(tracker_scan_start(&t, msg, sizeof(msg)) == NULL);
	for (int i = 0; i < 2000 && scan_running(&t.scan); i++)
		advance(1000);
	CHECK(t.scan.state == SCAN_DONE && t.scan.final_angle == 96);
	CHECK(servo_angle == 96 && t.link.status.commanded_angle == 96);
	CHECK(t.scan.results[0].angle == 45); /* baseline at the known start position */
	CHECK(t.link.n_malformed == 0 && t.link.n_stale == 0 && t.link.n_timeouts == 0);

	CHECK(tracker_move(&t, 150, msg, sizeof(msg)) == NULL);
	advance(50);
	CHECK(tracker_stop(&t, msg, sizeof(msg)) == NULL);
	advance(100);
	CHECK(t.link.status.interrupted && !t.link.status.output_enabled);
}
