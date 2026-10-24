/*
 * End-to-end scenarios: tracker + link + motion + scan against the simulated controller
 * and a synthetic signal field with a known best angle.
 */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "sim.h"
#include "tracker.h"

struct rig {
	struct tracker t;
	struct sim_transport tr;
	struct sim_controller dev;
	struct sim_sampler smp;
	struct st_config cfg;
};

static struct rig r;
static char msg[160];

static void rig_sample_done(void *ctx, const struct signal_sample *s)
{
	tracker_sample_done(ctx, s);
}

static void rig_setup(int best_angle, double noise)
{
	struct tracker_sampler ts = { .start = sim_sampler_start, .ctx = &r.smp };

	ev_cancel_all();
	memset(&r, 0, sizeof(r));
	sim_controller_init(&r.dev, 0, 180);
	sim_sampler_init(&r.smp, &r.dev, best_angle, rig_sample_done, &r.t);
	r.smp.signal.noise = noise;
	r.smp.signal.width = 15;
	tracker_init(&r.t, &sim_transport_ops, &r.tr, &ts);
	sim_transport_init(&r.tr, &r.t.link, sim_controller_handle, &r.dev, 20);

	config_defaults(&r.cfg);
	r.cfg.settle_ms = 500;
	r.cfg.samples = 3;
	r.cfg.min_valid_samples = 2;
	r.cfg.sample_interval_ms = 200;
	tracker_set_config(&r.t, &r.cfg, NULL);

	sim_transport_connect(&r.tr);
	advance(100);
}

static void apply_config(void)
{
	char err[160];
	bool ok = config_validate(&r.cfg, err, sizeof(err));

	tracker_set_config(&r.t, &r.cfg, ok ? NULL : err);
}

/* Runs until the scan leaves the running state (or a generous time limit passes). */
static void run_scan(void)
{
	for (int i = 0; i < 2000 && scan_running(&r.t.scan); i++)
		advance(1000);
}

static void test_connect(void)
{
	rig_setup(124, 0);
	CHECK(r.t.link.state == ESP_LINK_UP);
	CHECK(r.t.link.status.has_limits && r.t.link.status.max_angle == 180);
	CHECK(!r.t.link.status.has_angle); /* nothing commanded, no homing move */
	CHECK(!r.t.link.status.position_feedback);
}

static void test_manual_move(void)
{
	rig_setup(124, 0);

	CHECK(tracker_move(&r.t, 181, msg, sizeof(msg)) != NULL);
	CHECK(strcmp(tracker_move(&r.t, -1, msg, sizeof(msg)), "out_of_range") == 0);
	CHECK(!r.dev.has_angle);

	CHECK(tracker_move(&r.t, 90, msg, sizeof(msg)) == NULL);
	CHECK(r.t.motion.active);
	/* conflicting commands while moving */
	CHECK(strcmp(tracker_move(&r.t, 100, msg, sizeof(msg)), "busy") == 0);
	CHECK(strcmp(tracker_scan_start(&r.t, msg, sizeof(msg)), "busy") == 0);

	advance(3000);
	CHECK(!r.t.motion.active && r.t.last_move_error[0] == '\0');
	CHECK(r.dev.angle == 90 && r.t.link.status.commanded_angle == 90);

	/* configured range is narrower than the controller range */
	r.cfg.min_angle = 20;
	r.cfg.max_angle = 160;
	apply_config();
	CHECK(strcmp(tracker_move(&r.t, 170, msg, sizeof(msg)), "out_of_range") == 0);

	/* invalid configuration blocks movement */
	r.cfg.fine_step = 50;
	apply_config();
	CHECK(strcmp(tracker_move(&r.t, 100, msg, sizeof(msg)), "config_invalid") == 0);
	CHECK(strcmp(tracker_scan_start(&r.t, msg, sizeof(msg)), "config_invalid") == 0);
}

static void test_stop(void)
{
	rig_setup(124, 0);
	CHECK(tracker_move(&r.t, 170, msg, sizeof(msg)) == NULL);
	advance(150);
	CHECK(tracker_stop(&r.t, msg, sizeof(msg)) == NULL);
	advance(500);
	CHECK(!r.dev.output && r.dev.interrupted);
	CHECK(r.t.link.status.interrupted && !r.t.link.status.output_enabled);
	CHECK(!r.t.motion.active);
}

static void test_motion_timeout(void)
{
	rig_setup(124, 0);
	r.dev.ms_per_60deg = 100000; /* servo never reports the move finished */
	r.cfg.move_timeout_ms = 2000;
	apply_config();
	CHECK(tracker_move(&r.t, 90, msg, sizeof(msg)) == NULL);
	advance(5000);
	CHECK(!r.t.motion.active && strstr(r.t.last_move_error, "timeout"));
}

static void test_scan_finds_known_best(void)
{
	rig_setup(124, 0);
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	CHECK(scan_running(&r.t.scan));
	CHECK(strcmp(tracker_scan_start(&r.t, msg, sizeof(msg)), "scan_running") == 0);
	CHECK(strcmp(tracker_move(&r.t, 10, msg, sizeof(msg)), "scan_running") == 0);

	run_scan();
	CHECK(r.t.scan.state == SCAN_DONE && r.t.scan.complete);
	CHECK(r.t.scan.has_final && r.t.scan.final_angle == 124);
	CHECK(r.t.link.status.commanded_angle == 124 && r.dev.angle == 124);
	CHECK(scan_progress(&r.t.scan) == 100);
	/* start position unknown: no baseline; 19 coarse + fine 112..128 step 2 minus 120 */
	CHECK(r.t.scan.baseline_idx == -1);
	CHECK(r.t.scan.n_results == 19 + 8);
	CHECK(!r.t.scan.cell_changes);
}

static void test_scan_with_noise(void)
{
	rig_setup(73, 1.0);
	r.smp.signal.width = 40;
	r.cfg.samples = 5;
	r.cfg.min_valid_samples = 3;
	apply_config();
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	run_scan();
	CHECK(r.t.scan.state == SCAN_DONE);
	/* the side lobe at 163 deg must not win; noise allows a few degrees of error */
	CHECK(abs(r.t.scan.final_angle - 73) <= 8);
}

static void test_scan_baseline_and_threshold(void)
{
	rig_setup(124, 0);
	CHECK(tracker_move(&r.t, 60, msg, sizeof(msg)) == NULL);
	advance(3000);

	r.cfg.min_improvement = 100; /* impossible to beat: keep the starting angle */
	apply_config();
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	CHECK(r.t.scan.phase == SCAN_PHASE_BASELINE);
	run_scan();
	CHECK(r.t.scan.state == SCAN_DONE && r.t.scan.kept_baseline);
	CHECK(r.t.scan.baseline_idx == 0 && r.t.scan.results[0].angle == 60);
	CHECK(r.t.scan.final_angle == 60 && r.dev.angle == 60);
	CHECK(r.t.scan.results[r.t.scan.best_idx].angle == 124);

	r.cfg.min_improvement = 5;
	apply_config();
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	run_scan();
	CHECK(r.t.scan.state == SCAN_DONE && !r.t.scan.kept_baseline);
	CHECK(r.t.scan.final_angle == 124);
}

static void test_scan_cancel(void)
{
	int angle_after_cancel;

	rig_setup(124, 0);
	CHECK(tracker_scan_cancel(&r.t, msg, sizeof(msg)) != NULL);
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	advance(9000);
	CHECK(r.t.scan.n_results >= 2);
	CHECK(tracker_scan_cancel(&r.t, msg, sizeof(msg)) == NULL);
	CHECK(r.t.scan.state == SCAN_CANCELLED && !r.t.scan.complete);
	CHECK(r.t.scan.n_results >= 2 && r.t.scan.best_idx >= 0); /* partial results kept */

	advance(2000); /* an in-flight move may finish, nothing new starts */
	angle_after_cancel = r.dev.angle;
	advance(30000);
	CHECK(r.dev.angle == angle_after_cancel);
	CHECK(!r.t.scan.has_final);

	/* the tracker is usable again */
	CHECK(tracker_move(&r.t, 10, msg, sizeof(msg)) == NULL);
	advance(3000);
	CHECK(r.dev.angle == 10);

	/* stop also cancels a running scan */
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	advance(3000);
	CHECK(tracker_stop(&r.t, msg, sizeof(msg)) == NULL);
	CHECK(r.t.scan.state == SCAN_CANCELLED);
}

static void test_scan_missing_measurements(void)
{
	rig_setup(124, 0);
	r.smp.signal.fail = true;
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	run_scan();
	CHECK(r.t.scan.state == SCAN_FAILED);
	CHECK(strstr(r.t.scan.message, "No valid signal") != NULL);
	CHECK(r.t.scan.n_results == SCAN_MAX_UNSCORED && r.t.scan.best_idx == -1);
	CHECK(!r.t.scan.has_final);

	/* modem without SINR: scan still works on RSRP/RSRQ */
	rig_setup(124, 0);
	r.smp.signal.no_sinr = true;
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	run_scan();
	CHECK(r.t.scan.state == SCAN_DONE && r.t.scan.final_angle == 124);
	CHECK(!r.t.scan.results[0].has_sinr && r.t.scan.results[0].scored);
}

static void test_scan_disconnect_and_reconnect(void)
{
	int moves_before;

	rig_setup(124, 0);
	r.tr.reconnect_ms = 3000;
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	while (r.t.scan.n_results < 3)
		advance(500);
	sim_transport_unplug(&r.tr);
	CHECK(r.t.link.state == ESP_LINK_DOWN);
	CHECK(r.t.scan.state == SCAN_FAILED && strstr(r.t.scan.message, "disconnected"));
	CHECK(r.t.scan.n_results >= 3 && r.t.scan.best_idx >= 0 && !r.t.scan.has_final);
	CHECK(strcmp(tracker_move(&r.t, 50, msg, sizeof(msg)), "not_connected") == 0);
	CHECK(strcmp(tracker_scan_start(&r.t, msg, sizeof(msg)), "not_connected") == 0);

	moves_before = r.dev.angle;
	advance(4000);
	CHECK(r.t.link.state == ESP_LINK_UP);
	CHECK(r.dev.angle == moves_before); /* reconnecting never moves the servo */
	CHECK(tracker_move(&r.t, 50, msg, sizeof(msg)) == NULL);
	advance(3000);
	CHECK(r.dev.angle == 50);
}

static void test_scan_controller_reboot(void)
{
	char boot[200];

	rig_setup(124, 0);
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	advance(6000);
	sim_controller_boot(&r.dev, boot, sizeof(boot));
	sim_transport_inject(&r.tr, boot);
	advance(100);
	CHECK(r.t.scan.state == SCAN_FAILED && strstr(r.t.scan.message, "rebooted"));
	CHECK(!r.t.link.status.has_angle);
	CHECK(r.t.link.state == ESP_LINK_UP);
}

static void test_scan_unresponsive_controller(void)
{
	rig_setup(124, 0);
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	advance(3000);
	r.tr.mute = true;
	advance(10000);
	CHECK(r.t.scan.state == SCAN_FAILED);
	CHECK(r.tr.resets >= 1 && r.t.link.state == ESP_LINK_DOWN);
}

static void test_poll_and_sampler_sharing(void)
{
	int guard = 0;

	rig_setup(124, 0);
	tracker_start_polling(&r.t);
	advance(1000);
	CHECK(r.t.has_signal && r.t.last_signal.has_rsrp && !r.t.sampling);

	CHECK(tracker_move(&r.t, 60, msg, sizeof(msg)) == NULL);
	advance(3000);

	/* wait for the next idle poll to start measuring */
	while (!(r.t.sampling && r.t.sample_origin == TRACKER_SAMPLE_POLL) && guard++ < 1000)
		advance(10);
	CHECK(r.t.sampling && r.t.sample_origin == TRACKER_SAMPLE_POLL);

	/* the baseline sample is requested at once and must queue behind the poll */
	CHECK(tracker_scan_start(&r.t, msg, sizeof(msg)) == NULL);
	advance(1);
	CHECK(r.t.scan_sample_queued);
	advance(400);
	CHECK(!r.t.scan_sample_queued && r.t.sample_origin == TRACKER_SAMPLE_SCAN);
	run_scan();
	CHECK(r.t.scan.state == SCAN_DONE && r.t.scan.final_angle == 124);
	CHECK(r.t.scan.results[0].angle == 60 && r.t.scan.results[0].scored);
}

void run_scenario_tests(void)
{
	test_connect();
	test_manual_move();
	test_stop();
	test_motion_timeout();
	test_scan_finds_known_best();
	test_scan_with_noise();
	test_scan_baseline_and_threshold();
	test_scan_cancel();
	test_scan_missing_measurements();
	test_scan_disconnect_and_reconnect();
	test_scan_controller_reboot();
	test_scan_unresponsive_controller();
	test_poll_and_sampler_sharing();
}
