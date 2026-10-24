/*
 * Servo controller protocol logic (newline-delimited JSON over USB serial).
 *
 * Hardware independent: the servo is reached through struct servo_ops and time is
 * passed in by the caller, so this file also builds and runs in host unit tests.
 */
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONTROLLER_FW_VERSION "1.0.0"
#define CONTROLLER_LINE_MAX 160     /* longest accepted request line */
#define CONTROLLER_RESPONSE_MAX 256 /* buffer size for any response line */

struct servo_ops {
	int (*set_angle)(void *ctx, int angle); /* returns 0 on success */
	int (*disable)(void *ctx);              /* stop driving the servo signal */
	void *ctx;
};

struct controller_config {
	int min_angle;    /* mechanical limits of the platform, degrees */
	int max_angle;
	int travel_deg;   /* full servo travel, used when the start position is unknown */
	int ms_per_60deg; /* servo speed, used to estimate move duration */
	int settle_ms;    /* added to every move estimate */
};

struct controller {
	struct controller_config cfg;
	struct servo_ops servo;
	bool has_angle;      /* a position has been commanded since boot */
	int angle;           /* last commanded angle */
	bool output_enabled; /* PWM signal is being generated */
	bool interrupted;    /* last move was stopped before its estimated end */
	int64_t move_end_ms;
};

void controller_init(struct controller *c, const struct controller_config *cfg,
		     const struct servo_ops *servo);
bool controller_moving(const struct controller *c, int64_t now_ms);

/* Each function writes one complete response line (with trailing '\n') and returns its length. */
size_t controller_boot_event(const struct controller *c, char *out, size_t cap);
size_t controller_handle_line(struct controller *c, const char *line, int64_t now_ms,
			      char *out, size_t cap);
size_t controller_error(bool has_id, int id, const char *code, const char *message,
			char *out, size_t cap);

#endif
