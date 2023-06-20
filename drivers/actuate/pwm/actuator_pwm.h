/*
 * Copyright CogniPilot Foundation 2023
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/drivers/pwm.h>

typedef enum pwm_type_t {
    PWM_TYPE_NORMALIZED = 0,
    PWM_TYPE_POSITION,
    PWM_TYPE_VELOCITY,
} pwm_type_t;

typedef struct actuator_pwm_t {
    uint16_t min;
    uint16_t max;
    uint16_t center;
    const char* alias;
    pwm_type_t type;
    float slope;
    float intercept;
    uint8_t index;
    struct pwm_dt_spec device;
} actuator_pwm_t;
