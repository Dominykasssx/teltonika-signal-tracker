/*
 * USB serial transport for the ESP32 controller (router only).
 *
 * Finds the controller by USB vendor:product id through sysfs, never by a fixed
 * /dev/ttyUSB0 name: the RUTX modem also exposes ttyUSB ports through the option driver.
 * The port is opened non-blocking and exclusively; when the device disappears it is
 * looked for again every few seconds.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>

#include <libubox/uloop.h>

#include "config.h"
#include "esp_link.h"

#define SERIAL_BAUD B115200
#define SERIAL_RETRY_MS 3000

struct serial_port {
	struct uloop_fd ufd;
	struct uloop_timeout retry;
	struct esp_link *link;
	const struct st_config *cfg;
	bool open;
	char path[64];
	char error[160];
	void (*log)(int level, const char *msg);
};

extern const struct esp_link_ops serial_ops;

void serial_init(struct serial_port *sp, struct esp_link *link, const struct st_config *cfg);
/* Opens the port now (or keeps retrying in the background). */
void serial_start(struct serial_port *sp);
/* Closes and reopens, e.g. after the device configuration changed. */
void serial_restart(struct serial_port *sp);
void serial_shutdown(struct serial_port *sp);

#endif
