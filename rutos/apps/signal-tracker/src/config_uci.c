#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uci.h>

#include "config.h"

#define UCI_CONFIG "signal_tracker"
#define UCI_SECTION "main"

static bool get_int(struct uci_context *ctx, struct uci_section *s, const char *name, int *out,
		    char *err, size_t len)
{
	const char *v = uci_lookup_option_string(ctx, s, name);
	char *end;
	long n;

	if (!v)
		return true; /* keep the default */
	errno = 0;
	n = strtol(v, &end, 10);
	if (errno || end == v || *end != '\0' || n < -1000000 || n > 1000000) {
		snprintf(err, len, "Option '%s' must be an integer", name);
		return false;
	}
	*out = (int)n;
	return true;
}

static bool get_double(struct uci_context *ctx, struct uci_section *s, const char *name,
		       double *out, char *err, size_t len)
{
	const char *v = uci_lookup_option_string(ctx, s, name);
	char *end;
	double n;

	if (!v)
		return true;
	n = strtod(v, &end);
	if (end == v || *end != '\0') {
		snprintf(err, len, "Option '%s' must be a number", name);
		return false;
	}
	*out = n;
	return true;
}

static void get_string(struct uci_context *ctx, struct uci_section *s, const char *name,
		       char *out, size_t size)
{
	const char *v = uci_lookup_option_string(ctx, s, name);

	if (v)
		snprintf(out, size, "%s", v);
}

static void get_usb_ids(struct uci_context *ctx, struct uci_section *s, struct st_config *c)
{
	struct uci_option *o = uci_lookup_option(ctx, s, "usb_id");
	struct uci_element *e;

	if (!o || o->type != UCI_TYPE_LIST)
		return;
	c->n_usb_ids = 0;
	uci_foreach_element(&o->v.list, e) {
		if (c->n_usb_ids < CONFIG_MAX_USB_IDS)
			snprintf(c->usb_ids[c->n_usb_ids++], sizeof(c->usb_ids[0]), "%s", e->name);
	}
}

int config_load(struct st_config *c, char *err, size_t len)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_section *s;
	int simulate = 0;
	bool ok = true;

	config_defaults(c);
	if (!ctx) {
		snprintf(err, len, "Out of memory");
		return -1;
	}
	if (uci_load(ctx, UCI_CONFIG, &pkg) != UCI_OK) {
		snprintf(err, len, "Cannot load /etc/config/" UCI_CONFIG);
		uci_free_context(ctx);
		return -1;
	}

	s = uci_lookup_section(ctx, pkg, UCI_SECTION);
	if (s) {
		ok = get_int(ctx, s, "min_angle", &c->min_angle, err, len) &&
		     get_int(ctx, s, "max_angle", &c->max_angle, err, len) &&
		     get_int(ctx, s, "coarse_step", &c->coarse_step, err, len) &&
		     get_int(ctx, s, "fine_step", &c->fine_step, err, len) &&
		     get_int(ctx, s, "settle_ms", &c->settle_ms, err, len) &&
		     get_int(ctx, s, "samples", &c->samples, err, len) &&
		     get_int(ctx, s, "sample_interval_ms", &c->sample_interval_ms, err, len) &&
		     get_int(ctx, s, "min_valid_samples", &c->min_valid_samples, err, len) &&
		     get_int(ctx, s, "move_timeout_ms", &c->move_timeout_ms, err, len) &&
		     get_int(ctx, s, "request_timeout_ms", &c->request_timeout_ms, err, len) &&
		     get_double(ctx, s, "min_improvement", &c->min_improvement, err, len) &&
		     get_double(ctx, s, "weight_rsrp", &c->weights.rsrp, err, len) &&
		     get_double(ctx, s, "weight_rsrq", &c->weights.rsrq, err, len) &&
		     get_double(ctx, s, "weight_sinr", &c->weights.sinr, err, len) &&
		     get_int(ctx, s, "simulate", &simulate, err, len) &&
		     get_int(ctx, s, "sim_best_angle", &c->sim_best_angle, err, len);
		get_string(ctx, s, "device", c->device, sizeof(c->device));
		get_string(ctx, s, "modem", c->modem, sizeof(c->modem));
		get_usb_ids(ctx, s, c);
		c->simulate = simulate != 0;
	}

	uci_unload(ctx, pkg);
	uci_free_context(ctx);

	if (!ok || !config_validate(c, err, len))
		return -1;
	return 0;
}
