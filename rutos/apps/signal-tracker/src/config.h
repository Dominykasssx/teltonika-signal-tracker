/*
 * Daemon configuration (UCI /etc/config/signal_tracker, section 'main').
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "score.h"

#define CONFIG_MAX_USB_IDS 8
#define CONFIG_MAX_SAMPLES 20

struct st_config {
	int min_angle;
	int max_angle;
	int coarse_step;
	int fine_step;
	int settle_ms;          /* wait after a move before sampling */
	int samples;            /* samples per angle */
	int sample_interval_ms; /* gap between samples at one angle */
	int min_valid_samples;
	int move_timeout_ms;
	double min_improvement; /* score points the best angle must beat the start angle by */
	struct score_weights weights;
	int request_timeout_ms; /* serial request timeout */
	char device[64];        /* "auto" or an explicit /dev path */
	int n_usb_ids;
	char usb_ids[CONFIG_MAX_USB_IDS][10]; /* "vvvv:pppp" accepted by auto-detection */
	char modem[32];         /* gsmctl -O modem USB id, empty for the default modem */
	bool simulate;          /* use the built-in controller and signal simulator */
	int sim_best_angle;
};

void config_defaults(struct st_config *c);
/* Checks ranges and cross-field rules; writes a human-readable reason on failure. */
bool config_validate(const struct st_config *c, char *err, size_t len);
/* Loads UCI (router only, config_uci.c); returns 0 on success. */
int config_load(struct st_config *c, char *err, size_t len);

#endif
