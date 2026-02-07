// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 Elia Chiarucci

#pragma once

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/gatt.h>

#define GPIO_NODE DT_NODELABEL(gpio0) // Assuming GPIO port 0
#define GPIO_STATUS_LED 7			  // Pin 7
#define GPIO_RELAY_STATUS 10		  // Pin 10
#define GPIO_RELAY_SET 8			  // Pin 8
#define GPIO_RELAY_RESET 9			  // Pin 9

static struct device *gpio_dev;
static struct k_timer relay_timer;

int get_relay_status()
{
	int err;
	err = gpio_pin_configure(gpio_dev, GPIO_RELAY_STATUS, GPIO_INPUT | GPIO_PULL_UP);
	if (err < 0)
	{
		LOG_INF("Failed to configure GPIO pin %d\n", GPIO_RELAY_STATUS);
		return err;
	}
	int relay_status = gpio_pin_get(gpio_dev, GPIO_RELAY_STATUS);
	err = gpio_pin_configure(gpio_dev, GPIO_RELAY_STATUS, GPIO_INPUT | GPIO_DISCONNECTED);
	if (err < 0)
	{
		LOG_INF("Failed to configure GPIO pin %d\n", GPIO_RELAY_STATUS);
		return err;
	}
	return relay_status;
}

void relay_timer_handler(struct k_timer *timer_id)
{
	gpio_pin_set(gpio_dev, GPIO_RELAY_SET, 0);
	gpio_pin_set(gpio_dev, GPIO_RELAY_RESET, 0);
	LOG_INF("DONE");
}

void relay_on()
{
	LOG_INF("TURNING ON");
	gpio_pin_set(gpio_dev, GPIO_RELAY_SET, 1);
	k_timer_start(&relay_timer, K_MSEC(10), K_NO_WAIT); // Schedule to turn off in 10ms
}

void relay_off()
{
	LOG_INF("TURNING OFF");
	gpio_pin_set(gpio_dev, GPIO_RELAY_RESET, 1);
	k_timer_start(&relay_timer, K_MSEC(10), K_NO_WAIT); // Schedule to turn off in 10ms
}

static ssize_t read_relay(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
						  uint16_t len, uint16_t offset, uint8_t flags)
{
	int relay_status = get_relay_status();
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &relay_status, sizeof(relay_status));
}

static ssize_t set_relay(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
						 uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t value = *((uint8_t *)buf);
	LOG_INF("Integer: %d.", value);
	if (value == 1)
	{
		relay_on();
	}
	else
	{
		relay_off();
	}

	return len;
}

int gpio_start()
{
	int err;
	k_timer_init(&relay_timer, relay_timer_handler, NULL);
	
	/* Get device binding from the DeviceTree */
	gpio_dev = DEVICE_DT_GET(GPIO_NODE);
	if (!device_is_ready(gpio_dev))
	{
		LOG_INF("GPIO device is not ready\n");
		return -1;
	}

	/* Configure the pin as an output */
	err = gpio_pin_configure(gpio_dev, GPIO_STATUS_LED, GPIO_OUTPUT_INACTIVE);
	if (err < 0)
	{
		LOG_INF("Failed to configure GPIO pin %d\n", GPIO_STATUS_LED);
		return err;
	}

	err = gpio_pin_configure(gpio_dev, GPIO_RELAY_SET, GPIO_OUTPUT_INACTIVE);
	if (err < 0)
	{
		LOG_INF("Failed to configure GPIO pin %d\n", GPIO_RELAY_SET);
		return err;
	}

	err = gpio_pin_configure(gpio_dev, GPIO_RELAY_RESET, GPIO_OUTPUT_INACTIVE);
	if (err < 0)
	{
		LOG_INF("Failed to configure GPIO pin %d\n", GPIO_RELAY_RESET);
		return err;
	}
	err = gpio_pin_configure(gpio_dev, GPIO_RELAY_STATUS, GPIO_INPUT | GPIO_DISCONNECTED);
	if (err < 0)
	{
		LOG_INF("Failed to configure GPIO pin %d\n", GPIO_RELAY_STATUS);
		return err;
	}

	LOG_INF("GPIO started successfully!");
	return 0;
}
