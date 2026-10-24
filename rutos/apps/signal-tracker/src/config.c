#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* USB-UART bridges used on ESP32 boards, plus native USB CDC (Espressif VID). */
static const char *const default_usb_ids[] = {
	"10c4:ea60", /* Silicon Labs CP210x */
	"1a86:7523", /* WCH CH340 */
	"1a86:55d4", /* WCH CH9102 */
	"0403:6001", /* FTDI FT232R */
	"0403:6015", /* FTDI FT231X */
	"303a:1001", /* Espressif USB-Serial/JTAG (S3/C3 native USB) */
};

void config_defaults(struct st_config *c)
{
	memset(c, 0, sizeof(*c));
	c->min_angle = 0;
	c->max_angle = 180;
	c->coarse_step = 10;
	c->fine_step = 2;
	c->settle_ms = 4000;
	c->samples = 5;
	c->sample_interval_ms = 2000;
	c->min_valid_samples = 3;
	c->move_timeout_ms = 5000;
	c->min_improvement = 0;
	c->weights.rsrp = 0.5;
	c->weights.rsrq = 0.2;
	c->weights.sinr = 0.3;
	c->request_timeout_ms = 1000;
	strcpy(c->device, "auto");
	for (size_t i = 0; i < sizeof(default_usb_ids) / sizeof(default_usb_ids[0]); i++)
		strcpy(c->usb_ids[c->n_usb_ids++], default_usb_ids[i]);
	c->sim_best_angle = 120;
}

static bool valid_usb_id(const char *s)
{
	if (strlen(s) != 9 || s[4] != ':')
		return false;
	for (int i = 0; i < 9; i++) {
		if (i != 4 && !isxdigit((unsigned char)s[i]))
			return false;
	}
	return true;
}

#define CHECK_RANGE(field, lo, hi, what)                                                   \
	do {                                                                               \
		if (c->field < (lo) || c->field > (hi)) {                                  \
			snprintf(err, len, "%s must be between %d and %d", what, (int)(lo), \
				 (int)(hi));                                               \
			return false;                                                      \
		}                                                                          \
	} while (0)

bool config_validate(const struct st_config *c, char *err, size_t len)
{
	CHECK_RANGE(min_angle, 0, 360, "Minimum angle");
	CHECK_RANGE(max_angle, 0, 360, "Maximum angle");
	if (c->min_angle >= c->max_angle) {
		snprintf(err, len, "Minimum angle must be smaller than maximum angle");
		return false;
	}
	CHECK_RANGE(coarse_step, 1, c->max_angle - c->min_angle, "Coarse step");
	CHECK_RANGE(fine_step, 1, c->coarse_step, "Fine step");
	CHECK_RANGE(settle_ms, 0, 120000, "Settling delay (ms)");
	CHECK_RANGE(samples, 1, CONFIG_MAX_SAMPLES, "Samples per angle");
	CHECK_RANGE(sample_interval_ms, 0, 60000, "Sample interval (ms)");
	CHECK_RANGE(min_valid_samples, 1, c->samples, "Minimum valid samples");
	CHECK_RANGE(move_timeout_ms, 500, 60000, "Movement timeout (ms)");
	CHECK_RANGE(request_timeout_ms, 200, 10000, "Request timeout (ms)");
	if (c->min_improvement < 0 || c->min_improvement > 100) {
		snprintf(err, len, "Minimum improvement must be between 0 and 100 points");
		return false;
	}
	if (c->weights.rsrp < 0 || c->weights.rsrq < 0 || c->weights.sinr < 0 ||
	    c->weights.rsrp + c->weights.rsrq + c->weights.sinr <= 0) {
		snprintf(err, len, "Score weights must be non-negative with a positive sum");
		return false;
	}
	if (strcmp(c->device, "auto") != 0 && strncmp(c->device, "/dev/", 5) != 0) {
		snprintf(err, len, "Device must be 'auto' or a /dev path");
		return false;
	}
	for (int i = 0; i < c->n_usb_ids; i++) {
		if (!valid_usb_id(c->usb_ids[i])) {
			snprintf(err, len, "Invalid USB id '%s' (expected vvvv:pppp)", c->usb_ids[i]);
			return false;
		}
	}
	if (strcmp(c->device, "auto") == 0 && c->n_usb_ids == 0 && !c->simulate) {
		snprintf(err, len, "No USB ids configured for device auto-detection");
		return false;
	}
	CHECK_RANGE(sim_best_angle, 0, 360, "Simulated best angle");
	return true;
}
