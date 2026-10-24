/*
 * Hobby servo driver (50 Hz PWM through LEDC). No position feedback.
 * Replace this file to support a different actuator; controller.c only uses these calls.
 */
#ifndef SERVO_H
#define SERVO_H

#include "esp_err.h"

/* Configures the PWM peripheral with the output idle (no pulses). */
esp_err_t servo_init(void);
/* Starts or updates the pulse train for the given angle (0..CONFIG_SERVO_TRAVEL_DEG). */
esp_err_t servo_set_angle(int angle);
/* Stops generating pulses; most hobby servos then stop holding position. */
esp_err_t servo_disable(void);

#endif
