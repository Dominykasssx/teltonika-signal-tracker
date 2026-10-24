#include "controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flatjson.h"

void controller_init(struct controller *c, const struct controller_config *cfg,
		     const struct servo_ops *servo)
{
	memset(c, 0, sizeof(*c));
	c->cfg = *cfg;
	c->servo = *servo;
}

bool controller_moving(const struct controller *c, int64_t now_ms)
{
	return c->output_enabled && now_ms < c->move_end_ms;
}

static size_t finish(char *out, size_t cap, int len)
{
	if (len < 0 || (size_t)len >= cap) {
		/* Cannot happen with CONTROLLER_RESPONSE_MAX buffers; never emit a torn line. */
		if (cap > 0)
			out[0] = '\0';
		return 0;
	}
	return (size_t)len;
}

static int id_prefix(char *buf, size_t cap, bool has_id, int id)
{
	return has_id ? snprintf(buf, cap, "\"id\":%d,", id) : snprintf(buf, cap, "%s", "");
}

static void angle_json(const struct controller *c, char *buf, size_t cap)
{
	if (c->has_angle)
		snprintf(buf, cap, "%d", c->angle);
	else
		snprintf(buf, cap, "null");
}

size_t controller_error(bool has_id, int id, const char *code, const char *message,
			char *out, size_t cap)
{
	char idbuf[24];

	id_prefix(idbuf, sizeof(idbuf), has_id, id);
	return finish(out, cap,
		      snprintf(out, cap, "{%s\"status\":\"error\",\"error\":\"%s\",\"message\":\"%s\"}\n",
			       idbuf, code, message));
}

size_t controller_boot_event(const struct controller *c, char *out, size_t cap)
{
	return finish(out, cap,
		      snprintf(out, cap,
			       "{\"event\":\"boot\",\"fw\":\"%s\",\"min_angle\":%d,\"max_angle\":%d,"
			       "\"position_feedback\":false}\n",
			       CONTROLLER_FW_VERSION, c->cfg.min_angle, c->cfg.max_angle));
}

static size_t handle_status(const struct controller *c, bool has_id, int id, int64_t now,
			    char *out, size_t cap)
{
	char idbuf[24], angle[12];
	bool moving = controller_moving(c, now);
	const char *state = moving ? "moving" : (c->output_enabled ? "holding" : "released");

	id_prefix(idbuf, sizeof(idbuf), has_id, id);
	angle_json(c, angle, sizeof(angle));
	return finish(out, cap,
		      snprintf(out, cap,
			       "{%s\"status\":\"ok\",\"state\":\"%s\",\"commanded_angle\":%s,"
			       "\"moving\":%s,\"output_enabled\":%s,\"interrupted\":%s,"
			       "\"position_feedback\":false,\"min_angle\":%d,\"max_angle\":%d}\n",
			       idbuf, state, angle, moving ? "true" : "false",
			       c->output_enabled ? "true" : "false", c->interrupted ? "true" : "false",
			       c->cfg.min_angle, c->cfg.max_angle));
}

static int estimate_move_ms(const struct controller *c, int target)
{
	int distance;

	/* Without feedback the start position is only known if we commanded it and it completed. */
	if (c->has_angle && !c->interrupted)
		distance = abs(target - c->angle);
	else
		distance = c->cfg.travel_deg;

	if (distance == 0 && c->output_enabled)
		return 0;
	return distance * c->cfg.ms_per_60deg / 60 + c->cfg.settle_ms;
}

static size_t handle_set_position(struct controller *c, const struct fj_object *req, bool has_id,
				  int id, int64_t now, char *out, size_t cap)
{
	char idbuf[24], msg[64];
	const struct fj_field *f = fj_get(req, "angle");
	int angle, move_ms;

	if (!f)
		return controller_error(has_id, id, "missing_angle", "angle is required", out, cap);
	if (!fj_get_int(req, "angle", &angle))
		return controller_error(has_id, id, "invalid_angle", "angle must be an integer",
					out, cap);
	if (angle < c->cfg.min_angle || angle > c->cfg.max_angle) {
		snprintf(msg, sizeof(msg), "angle must be between %d and %d", c->cfg.min_angle,
			 c->cfg.max_angle);
		return controller_error(has_id, id, "angle_out_of_range", msg, out, cap);
	}
	if (controller_moving(c, now))
		return controller_error(has_id, id, "busy", "a move is in progress", out, cap);

	move_ms = estimate_move_ms(c, angle);
	if (c->servo.set_angle(c->servo.ctx, angle) != 0)
		return controller_error(has_id, id, "servo_error", "failed to drive servo", out, cap);

	c->has_angle = true;
	c->angle = angle;
	c->output_enabled = true;
	c->interrupted = false;
	c->move_end_ms = now + move_ms;

	id_prefix(idbuf, sizeof(idbuf), has_id, id);
	return finish(out, cap,
		      snprintf(out, cap,
			       "{%s\"status\":\"ok\",\"commanded_angle\":%d,\"moving\":%s,"
			       "\"move_time_ms\":%d}\n",
			       idbuf, angle, move_ms > 0 ? "true" : "false", move_ms));
}

static size_t handle_stop(struct controller *c, bool has_id, int id, int64_t now, char *out,
			  size_t cap)
{
	char idbuf[24], angle[12];

	if (controller_moving(c, now))
		c->interrupted = true;
	if (c->servo.disable(c->servo.ctx) != 0)
		return controller_error(has_id, id, "servo_error", "failed to stop servo", out, cap);
	c->output_enabled = false;
	c->move_end_ms = now;

	id_prefix(idbuf, sizeof(idbuf), has_id, id);
	angle_json(c, angle, sizeof(angle));
	return finish(out, cap,
		      snprintf(out, cap,
			       "{%s\"status\":\"ok\",\"commanded_angle\":%s,\"moving\":false,"
			       "\"output_enabled\":false,\"interrupted\":%s}\n",
			       idbuf, angle, c->interrupted ? "true" : "false"));
}

size_t controller_handle_line(struct controller *c, const char *line, int64_t now_ms,
			      char *out, size_t cap)
{
	struct fj_object req;
	const char *cmd;
	bool has_id = false;
	int id = 0;

	if (fj_parse(line, &req) != FJ_OK)
		return controller_error(false, 0, "invalid_json",
					"expected a flat JSON object", out, cap);

	if (fj_get(&req, "id")) {
		if (!fj_get_int(&req, "id", &id) || id < 0)
			return controller_error(false, 0, "invalid_id",
						"id must be a non-negative integer", out, cap);
		has_id = true;
	}

	if (!fj_get_string(&req, "command", &cmd))
		return controller_error(has_id, id, "missing_command", "command is required",
					out, cap);

	if (strcmp(cmd, "get_status") == 0)
		return handle_status(c, has_id, id, now_ms, out, cap);
	if (strcmp(cmd, "set_position") == 0)
		return handle_set_position(c, &req, has_id, id, now_ms, out, cap);
	if (strcmp(cmd, "stop") == 0)
		return handle_stop(c, has_id, id, now_ms, out, cap);

	return controller_error(has_id, id, "unknown_command", "unknown command", out, cap);
}
