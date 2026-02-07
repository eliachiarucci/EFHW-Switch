// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 Elia Chiarucci

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "gpio_manager.h"

static struct k_timer led_timer;

typedef struct
{
    int duration;
    bool state;
} led_blink_step_t;

#define low_battery_led_steps_size 2
const led_blink_step_t low_battery_led_steps[low_battery_led_steps_size] = {
    {.duration = 75, .state = true},
    {.duration = 5000, .state = false},
};

#define advertising_led_steps_size 6
const led_blink_step_t advertising_led_steps[advertising_led_steps_size] = {
    {.duration = 300, .state = true},
    {.duration = 150, .state = false},
    {.duration = 300, .state = true},
    {.duration = 150, .state = false},
    {.duration = 300, .state = true},
    {.duration = 5000, .state = false},
};

#define connected_led_steps_size 2
const led_blink_step_t connected_led_steps[connected_led_steps_size] = {
    {.duration = 750, .state = true},
    {.duration = 5000, .state = false},
};

enum led_blink_status
{
    BATTERY_LOW,
    ADVERTISING,
    CONNECTED
};

enum led_blink_status current_led_blink_status = ADVERTISING;

int led_timing_step = 0;

int get_timing(led_blink_step_t *timing)
{
    led_blink_step_t *led_steps;
    int led_steps_size;
    switch (current_led_blink_status)
    {
    case BATTERY_LOW:
        led_steps = low_battery_led_steps;
        led_steps_size = low_battery_led_steps_size;
        break;
    case ADVERTISING:
        led_steps = advertising_led_steps;
        led_steps_size = advertising_led_steps_size;
        break;
    case CONNECTED:
        led_steps = connected_led_steps;
        led_steps_size = connected_led_steps_size;
        break;
    default:
        return -1;
        break;
    }
    if (led_timing_step >= led_steps_size)
    {
        led_timing_step = 0;
    };
    *timing = led_steps[led_timing_step];
    led_timing_step++;
    return 0;
}

void timer_handler()
{
    // uint32_t pin_state = nrf_gpio_pin_out_read(GPIO_STATUS_LED);
    led_blink_step_t timing;
    int err = get_timing(&timing);
    if (err < 0)
    {
        LOG_ERR("Error getting timing");
        led_timing_step = 0;
        k_timer_start(&led_timer, K_MSEC(100), K_NO_WAIT);
        return;
    }
    int ms = timing.duration;
    gpio_pin_set(gpio_dev, GPIO_STATUS_LED, timing.state ? 1 : 0);
    k_timer_start(&led_timer, K_MSEC(ms), K_NO_WAIT);
}

void status_led_init()
{
    k_timer_init(&led_timer, timer_handler, NULL);
}

void status_led_update(enum led_blink_status status)
{
    led_timing_step = 0;
    current_led_blink_status = status;
}

void status_led_start(enum led_blink_status status)
{
    current_led_blink_status = status;
    timer_handler();
}