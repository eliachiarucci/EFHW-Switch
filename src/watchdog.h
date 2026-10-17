// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 Elia Chiarucci

#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>

#define WATCHDOG_TIMEOUT_MS 30000
#define WATCHDOG_FEED_INTERVAL_MS 5000

static const struct device *const wdt_dev = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int wdt_channel = -1;

// Starts the hardware watchdog: the SoC resets if it isn't fed for WATCHDOG_TIMEOUT_MS.
// Once started it can't be stopped and keeps running across soft reboots (MCUboot feeds it).
int watchdog_start()
{
	if (!device_is_ready(wdt_dev))
	{
		LOG_ERR("Watchdog device not ready");
		return -1;
	}

	struct wdt_timeout_cfg cfg = {
		.window.min = 0,
		.window.max = WATCHDOG_TIMEOUT_MS,
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	wdt_channel = wdt_install_timeout(wdt_dev, &cfg);
	if (wdt_channel < 0)
	{
		LOG_ERR("Watchdog install failed (err %d)", wdt_channel);
		return wdt_channel;
	}

	// Keeps counting while the CPU sleeps; pauses only while a debugger halts it
	int err = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err < 0)
	{
		LOG_ERR("Watchdog setup failed (err %d)", err);
		wdt_channel = -1;
		return err;
	}

	LOG_INF("Watchdog started (%d ms)", WATCHDOG_TIMEOUT_MS);
	return 0;
}

void watchdog_feed()
{
	if (wdt_channel >= 0)
	{
		wdt_feed(wdt_dev, wdt_channel);
	}
}
