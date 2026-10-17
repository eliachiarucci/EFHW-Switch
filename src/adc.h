// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 Elia Chiarucci

#pragma once

#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

static const struct adc_dt_spec adc_channel = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

int16_t buf; // Buffer for ADC result
struct adc_sequence sequence = {
	.buffer = &buf,
	.buffer_size = sizeof(buf),
};

float get_battery_voltage()
{
	adc_read_dt(&adc_channel, &sequence);
	float voltage_value = (buf * 3.6) / 4096 + 0.05;
	return voltage_value;
}

int adc_start()
{
	int err;
	if (!adc_is_ready_dt(&adc_channel))
	{
		LOG_ERR("ADC controller devivce %s not ready", adc_channel.dev->name);
		return 0;
	}

	err = adc_channel_setup_dt(&adc_channel);
	if (err < 0)
	{
		LOG_ERR("Could not setup channel #%d (%d)", 2, err);
		return 0;
	}
	else
	{
		LOG_INF("Correctly set up ADC!");
	}

	err = adc_sequence_init_dt(&adc_channel, &sequence);
	if (err < 0)
	{
		LOG_ERR("Could not initalize sequnce");
		return 0;
	}
}

// Read handler for the battery level characteristic
static ssize_t read_battery_level(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
								  uint16_t len, uint16_t offset)
{
	int16_t battery_voltage = get_battery_voltage() * 100;
	// Simply return the battery level (percent)
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &battery_voltage,
							 sizeof(battery_voltage));
}
