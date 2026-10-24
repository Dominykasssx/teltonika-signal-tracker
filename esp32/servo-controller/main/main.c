/*
 * ESP32 servo controller for the RUTX signal tracker.
 *
 * Talks newline-delimited JSON on UART0, which the classic ESP32 dev boards expose
 * through their on-board USB-to-UART bridge (CP210x / CH340). The console is disabled
 * in sdkconfig.defaults so log output never mixes with protocol responses.
 */
#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "controller.h"
#include "servo.h"

#define PROTO_UART UART_NUM_0
#define RX_BUF_SIZE 1024
#define TX_BUF_SIZE 1024
/* Drop a partial line if the host goes quiet mid-line (e.g. USB unplugged while sending). */
#define PARTIAL_LINE_TIMEOUT_MS 500

_Static_assert(CONFIG_SERVO_MIN_ANGLE < CONFIG_SERVO_MAX_ANGLE &&
		       CONFIG_SERVO_MAX_ANGLE <= CONFIG_SERVO_TRAVEL_DEG,
	       "servo angle limits must satisfy MIN < MAX <= TRAVEL");

static struct controller ctl;

static int64_t now_ms(void)
{
	return esp_timer_get_time() / 1000;
}

static int servo_set_cb(void *ctx, int angle)
{
	(void)ctx;
	return servo_set_angle(angle) == ESP_OK ? 0 : -1;
}

static int servo_disable_cb(void *ctx)
{
	(void)ctx;
	return servo_disable() == ESP_OK ? 0 : -1;
}

static void send_line(const char *line, size_t len)
{
	if (len > 0)
		uart_write_bytes(PROTO_UART, line, len);
}

static void uart_setup(void)
{
	const uart_config_t cfg = {
		.baud_rate = CONFIG_PROTO_BAUD_RATE,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	ESP_ERROR_CHECK(uart_driver_install(PROTO_UART, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(PROTO_UART, &cfg));
}

void app_main(void)
{
	const struct controller_config cfg = {
		.min_angle = CONFIG_SERVO_MIN_ANGLE,
		.max_angle = CONFIG_SERVO_MAX_ANGLE,
		.travel_deg = CONFIG_SERVO_TRAVEL_DEG,
		.ms_per_60deg = CONFIG_SERVO_MS_PER_60DEG,
		.settle_ms = CONFIG_SERVO_SETTLE_MS,
	};
	const struct servo_ops ops = {
		.set_angle = servo_set_cb,
		.disable = servo_disable_cb,
	};
	char line[CONTROLLER_LINE_MAX];
	char resp[CONTROLLER_RESPONSE_MAX];
	uint8_t rx[64];
	size_t len = 0;
	bool overflow = false;
	int64_t last_rx = 0;

	uart_setup();
	ESP_ERROR_CHECK(servo_init());
	controller_init(&ctl, &cfg, &ops);

	/* Lets the router notice a reboot: the commanded position is lost, no homing move is made. */
	send_line(resp, controller_boot_event(&ctl, resp, sizeof(resp)));

	for (;;) {
		int n = uart_read_bytes(PROTO_UART, rx, sizeof(rx), pdMS_TO_TICKS(50));

		if (n <= 0) {
			if ((len > 0 || overflow) && now_ms() - last_rx > PARTIAL_LINE_TIMEOUT_MS) {
				len = 0;
				overflow = false;
			}
			continue;
		}
		last_rx = now_ms();

		for (int i = 0; i < n; i++) {
			char c = (char)rx[i];

			if (c == '\r' || c == '\0')
				continue;
			if (c != '\n') {
				if (len + 1 < sizeof(line))
					line[len++] = c;
				else
					overflow = true;
				continue;
			}

			if (overflow) {
				send_line(resp, controller_error(false, 0, "line_too_long",
								 "request exceeds maximum length",
								 resp, sizeof(resp)));
			} else if (len > 0) {
				line[len] = '\0';
				send_line(resp, controller_handle_line(&ctl, line, now_ms(), resp,
								       sizeof(resp)));
			}
			len = 0;
			overflow = false;
		}
	}
}
