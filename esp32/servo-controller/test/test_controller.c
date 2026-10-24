/* Host unit tests for the firmware protocol logic. Build with: make -C test */
#include <stdio.h>
#include <string.h>

#include "controller.h"
#include "flatjson.h"

static int failures;
static int checks;

#define CHECK(cond)                                                              \
	do {                                                                     \
		checks++;                                                        \
		if (!(cond)) {                                                   \
			failures++;                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
		}                                                                \
	} while (0)

static int servo_angle = -1;
static int servo_enabled;
static int servo_fail;

static int fake_set(void *ctx, int angle)
{
	(void)ctx;
	if (servo_fail)
		return -1;
	servo_angle = angle;
	servo_enabled = 1;
	return 0;
}

static int fake_disable(void *ctx)
{
	(void)ctx;
	servo_enabled = 0;
	return 0;
}

static struct controller ctl;
static char resp[CONTROLLER_RESPONSE_MAX];
static struct fj_object obj;

static void setup(void)
{
	const struct controller_config cfg = {
		.min_angle = 10, .max_angle = 170, .travel_deg = 180,
		.ms_per_60deg = 300, .settle_ms = 100,
	};
	const struct servo_ops ops = { .set_angle = fake_set, .disable = fake_disable };

	controller_init(&ctl, &cfg, &ops);
	servo_angle = -1;
	servo_enabled = 0;
	servo_fail = 0;
}

/* Sends one request and parses the response line into obj. */
static void request(const char *line, long long now)
{
	size_t n = controller_handle_line(&ctl, line, now, resp, sizeof(resp));

	CHECK(n > 0 && resp[n - 1] == '\n');
	resp[n - 1] = '\0';
	CHECK(fj_parse(resp, &obj) == FJ_OK);
}

static int str_is(const char *key, const char *want)
{
	const char *v;

	return fj_get_string(&obj, key, &v) && strcmp(v, want) == 0;
}

static int int_is(const char *key, int want)
{
	int v;

	return fj_get_int(&obj, key, &v) && v == want;
}

static void test_flatjson(void)
{
	struct fj_object o;
	const char *s;
	bool b;
	int i;

	CHECK(fj_parse("{\"a\":1,\"b\":\"x\\\"y\",\"c\":true,\"d\":null,\"e\":-2.5e1}", &o) == FJ_OK);
	CHECK(o.count == 5);
	CHECK(fj_get_int(&o, "a", &i) && i == 1);
	CHECK(fj_get_string(&o, "b", &s) && strcmp(s, "x\"y") == 0);
	CHECK(fj_get_bool(&o, "c", &b) && b);
	CHECK(fj_is_null(&o, "d"));
	CHECK(fj_get_int(&o, "e", &i) && i == -25);
	double d;
	CHECK(fj_get_number(&o, "e", &d) && d == -25.0);
	CHECK(!fj_get_number(&o, "b", &d));
	CHECK(fj_parse("  { }  ", &o) == FJ_OK && o.count == 0);
	CHECK(fj_parse("{\"a\":1.5}", &o) == FJ_OK && !fj_get_int(&o, "a", &i));

	CHECK(fj_parse("", &o) != FJ_OK);
	CHECK(fj_parse("{", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":}", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":1,}", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":01}", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":0x10}", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":nan}", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":\"unterminated}", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":\"bad\\q\"}", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":\"\\u12\"}", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":1} trailing", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":{\"b\":1}}", &o) == FJ_ERR_NESTED);
	CHECK(fj_parse("{\"a\":[1]}", &o) == FJ_ERR_NESTED);
	CHECK(fj_parse("[1,2]", &o) != FJ_OK);
	CHECK(fj_parse("{\"a\":1,\"a2\":2,\"a3\":3,\"a4\":4,\"a5\":5,\"a6\":6,\"a7\":7,\"a8\":8,"
		       "\"a9\":9,\"b1\":1,\"b2\":2,\"b3\":3,\"b4\":4,\"b5\":5,\"b6\":6,\"b7\":7,"
		       "\"b8\":8}", &o) == FJ_ERR_TOO_BIG);
}

static void test_boot_and_status(void)
{
	size_t n;

	setup();
	n = controller_boot_event(&ctl, resp, sizeof(resp));
	CHECK(n > 0);
	resp[n - 1] = '\0';
	CHECK(fj_parse(resp, &obj) == FJ_OK);
	CHECK(str_is("event", "boot"));
	CHECK(int_is("min_angle", 10) && int_is("max_angle", 170));

	request("{\"id\":7,\"command\":\"get_status\"}", 0);
	CHECK(int_is("id", 7));
	CHECK(str_is("status", "ok"));
	CHECK(fj_is_null(&obj, "commanded_angle")); /* nothing commanded yet, no homing */
	CHECK(str_is("state", "released"));
	bool fb = true;
	CHECK(fj_get_bool(&obj, "position_feedback", &fb) && !fb);
	CHECK(servo_angle == -1);
}

static void test_set_position(void)
{
	bool moving;
	int t;

	setup();
	request("{\"id\":1,\"command\":\"set_position\",\"angle\":90}", 1000);
	CHECK(str_is("status", "ok"));
	CHECK(int_is("commanded_angle", 90));
	CHECK(servo_angle == 90 && servo_enabled);
	/* Unknown start: assume full 180 deg travel: 180*300/60 + 100 */
	CHECK(fj_get_int(&obj, "move_time_ms", &t) && t == 1000);
	CHECK(fj_get_bool(&obj, "moving", &moving) && moving);

	/* Conflicting command while moving is rejected and the servo is not touched. */
	request("{\"id\":2,\"command\":\"set_position\",\"angle\":100}", 1500);
	CHECK(str_is("status", "error") && str_is("error", "busy") && int_is("id", 2));
	CHECK(servo_angle == 90);

	request("{\"command\":\"get_status\"}", 1500);
	CHECK(str_is("state", "moving"));
	request("{\"command\":\"get_status\"}", 2000);
	CHECK(str_is("state", "holding"));
	CHECK(fj_get_bool(&obj, "moving", &moving) && !moving);

	/* Known start: 30 deg -> 150 ms + 100 ms settle */
	request("{\"command\":\"set_position\",\"angle\":120}", 2000);
	CHECK(fj_get_int(&obj, "move_time_ms", &t) && t == 250);
	CHECK(fj_get(&obj, "id") == NULL);

	/* Same angle again: nothing to do */
	request("{\"command\":\"set_position\",\"angle\":120}", 3000);
	CHECK(fj_get_int(&obj, "move_time_ms", &t) && t == 0);
	CHECK(fj_get_bool(&obj, "moving", &moving) && !moving);
}

static void test_invalid_angles(void)
{
	setup();
	request("{\"id\":3,\"command\":\"set_position\",\"angle\":171}", 0);
	CHECK(str_is("error", "angle_out_of_range") && int_is("id", 3));
	request("{\"command\":\"set_position\",\"angle\":9}", 0);
	CHECK(str_is("error", "angle_out_of_range"));
	request("{\"command\":\"set_position\",\"angle\":-5}", 0);
	CHECK(str_is("error", "angle_out_of_range"));
	request("{\"command\":\"set_position\",\"angle\":45.5}", 0);
	CHECK(str_is("error", "invalid_angle"));
	request("{\"command\":\"set_position\",\"angle\":\"90\"}", 0);
	CHECK(str_is("error", "invalid_angle"));
	request("{\"command\":\"set_position\",\"angle\":1e10}", 0);
	CHECK(str_is("error", "invalid_angle"));
	request("{\"command\":\"set_position\"}", 0);
	CHECK(str_is("error", "missing_angle"));
	CHECK(servo_angle == -1);

	request("{\"command\":\"set_position\",\"angle\":10}", 0);
	CHECK(str_is("status", "ok"));
	request("{\"command\":\"set_position\",\"angle\":170}", 5000);
	CHECK(str_is("status", "ok"));
}

static void test_malformed(void)
{
	setup();
	request("hello", 0);
	CHECK(str_is("error", "invalid_json"));
	request("{\"command\":", 0);
	CHECK(str_is("error", "invalid_json"));
	request("{\"command\":{\"x\":1}}", 0);
	CHECK(str_is("error", "invalid_json"));
	request("{\"id\":-1,\"command\":\"get_status\"}", 0);
	CHECK(str_is("error", "invalid_id"));
	request("{\"id\":\"a\",\"command\":\"get_status\"}", 0);
	CHECK(str_is("error", "invalid_id"));
	request("{\"id\":4}", 0);
	CHECK(str_is("error", "missing_command") && int_is("id", 4));
	request("{\"id\":5,\"command\":\"spin\"}", 0);
	CHECK(str_is("error", "unknown_command") && int_is("id", 5));
	request("{\"command\":42}", 0);
	CHECK(str_is("error", "missing_command"));
	CHECK(servo_angle == -1);

	setup();
	servo_fail = 1;
	request("{\"command\":\"set_position\",\"angle\":90}", 0);
	CHECK(str_is("error", "servo_error"));
	request("{\"command\":\"get_status\"}", 0);
	CHECK(fj_is_null(&obj, "commanded_angle"));
}

static void test_stop(void)
{
	bool b;
	int t;

	setup();
	request("{\"command\":\"set_position\",\"angle\":90}", 0);
	request("{\"id\":9,\"command\":\"stop\"}", 200);
	CHECK(str_is("status", "ok") && int_is("id", 9));
	CHECK(fj_get_bool(&obj, "interrupted", &b) && b);
	CHECK(fj_get_bool(&obj, "output_enabled", &b) && !b);
	CHECK(int_is("commanded_angle", 90));
	CHECK(!servo_enabled);

	request("{\"command\":\"get_status\"}", 300);
	CHECK(str_is("state", "released"));

	/* After an interrupted move the start is unknown again: full travel estimate. */
	request("{\"command\":\"set_position\",\"angle\":100}", 400);
	CHECK(str_is("status", "ok"));
	CHECK(fj_get_int(&obj, "move_time_ms", &t) && t == 1000);

	/* Stop when idle is accepted and is not an interruption. */
	request("{\"command\":\"stop\"}", 5000);
	CHECK(fj_get_bool(&obj, "interrupted", &b) && !b);
	request("{\"command\":\"stop\"}", 5100);
	CHECK(str_is("status", "ok"));
}

int main(void)
{
	test_flatjson();
	test_boot_and_status();
	test_set_position();
	test_invalid_angles();
	test_malformed();
	test_stop();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
