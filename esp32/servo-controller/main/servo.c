#include "servo.h"

#include "driver/ledc.h"
#include "sdkconfig.h"

#define SERVO_MODE LEDC_LOW_SPEED_MODE
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_CHANNEL LEDC_CHANNEL_0
#define SERVO_RESOLUTION LEDC_TIMER_14_BIT
#define SERVO_FREQ_HZ 50
#define SERVO_PERIOD_US (1000000 / SERVO_FREQ_HZ)

esp_err_t servo_init(void)
{
	const ledc_timer_config_t timer = {
		.speed_mode = SERVO_MODE,
		.duty_resolution = SERVO_RESOLUTION,
		.timer_num = SERVO_TIMER,
		.freq_hz = SERVO_FREQ_HZ,
		.clk_cfg = LEDC_AUTO_CLK,
	};
	const ledc_channel_config_t channel = {
		.gpio_num = CONFIG_SERVO_GPIO,
		.speed_mode = SERVO_MODE,
		.channel = SERVO_CHANNEL,
		.timer_sel = SERVO_TIMER,
		.duty = 0, /* no pulses until the first set_position */
		.hpoint = 0,
	};
	esp_err_t err = ledc_timer_config(&timer);

	if (err != ESP_OK)
		return err;
	return ledc_channel_config(&channel);
}

static esp_err_t set_duty(uint32_t duty)
{
	esp_err_t err = ledc_set_duty(SERVO_MODE, SERVO_CHANNEL, duty);

	if (err != ESP_OK)
		return err;
	return ledc_update_duty(SERVO_MODE, SERVO_CHANNEL);
}

esp_err_t servo_set_angle(int angle)
{
	const uint32_t max_duty = (1u << SERVO_RESOLUTION) - 1;
	uint32_t pulse_us;

	if (angle < 0 || angle > CONFIG_SERVO_TRAVEL_DEG)
		return ESP_ERR_INVALID_ARG;

	pulse_us = CONFIG_SERVO_MIN_PULSE_US +
		   (uint32_t)angle * (CONFIG_SERVO_MAX_PULSE_US - CONFIG_SERVO_MIN_PULSE_US) /
			   CONFIG_SERVO_TRAVEL_DEG;
	return set_duty(pulse_us * max_duty / SERVO_PERIOD_US);
}

esp_err_t servo_disable(void)
{
	return set_duty(0);
}
