// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 Elia Chiarucci

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <soc.h>
#include <zephyr/settings/settings.h>
#include <nrfx_gpiote.h>

LOG_MODULE_REGISTER(ble_relay, LOG_LEVEL_DBG);

#include "storage_manager.h"
#include "gpio_manager.h"
#include "status_led.h"
#include "adc.h"
#include "ble_manager.h"

struct k_timer update_led_status_timer;

int main(void)
{
	storage_manager_init();
	init_hw_revision();
	gpio_start();
	adc_start();
	status_led_init();
	ble_start();
}