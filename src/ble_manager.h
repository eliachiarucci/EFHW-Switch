// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2026 Elia Chiarucci

#pragma once

#include <bluetooth/services/bms.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/toolchain.h>

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* Authorization code for Bond Management Service
 * In hex: {0x41, 0x42, 0x43, 0x44}
 */
static const uint8_t bms_auth_code[] = {'A', 'B', 'C', 'D'};

#define CUSTOM_SERVICE_UUID                                                    \
  BT_UUID_128_ENCODE(0x67d03ad6, 0x34ba, 0x450a, 0xa39c, 0x00805f9b34fb)

#define BT_UUID_AUTOMATION_IO BT_UUID_DECLARE_16(0x1815)
#define BT_UUID_DIGITAL BT_UUID_DECLARE_16(0x2A56)

#define BT_UUID_BATTERY BT_UUID_DECLARE_16(0x180F)
#define BT_UUID_BATTERY_LEVEL BT_UUID_DECLARE_16(0x2A19)
#define BT_UUID_PRESENTATION_FORMAT BT_UUID_DECLARE_16(0x2904)

#define BT_UUID_CUSTOM_DEVICE_NAME_VAL                                         \
  BT_UUID_128_ENCODE(0x397bfacd, 0xf6c0, 0x470e, 0x8c72, 0x5b17d34e1259)
#define BT_UUID_CUSTOM_DEVICE_NAME                                             \
  BT_UUID_DECLARE_128(BT_UUID_CUSTOM_DEVICE_NAME_VAL)

#define BT_UUID_DEVICE_INFO                                                    \
  BT_UUID_DECLARE_16(0x180A) // Device Information Service
#define BT_UUID_FIRMWARE_REVISION                                              \
  BT_UUID_DECLARE_16(0x2A26) // Firmware Revision String
#define BT_UUID_HARDWARE_REVISION                                              \
  BT_UUID_DECLARE_16(0x2A27) // Hardware Revision String
#define BT_UUID_MODEL_NUMBER                                                   \
  BT_UUID_DECLARE_16(0x2A24) // Model Number String0x2A24

static const struct bt_le_conn_param *conn_params = BT_LE_CONN_PARAM(
    CONFIG_BT_PERIPHERAL_PREF_MIN_INT, CONFIG_BT_PERIPHERAL_PREF_MAX_INT,
    CONFIG_BT_PERIPHERAL_PREF_LATENCY, CONFIG_BT_PERIPHERAL_PREF_TIMEOUT);

// Presentation Format Descriptor for Voltage Level (scaled by 100, unit: volts)
static const uint8_t voltage_level_cpf[7] = {
    0x10, // Format: unsigned 16-bit integer
    0x00, // Exponent: no scaling (1.0)
    0x00, // Unit: Volts (0x2703)
    0x03, // Unit: Volts (0x2703)
    0x00, // Additional flags (reserved)
    0x00, // Reserved
    0x00  // Reserved
};

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, CUSTOM_SERVICE_UUID),
};

static ssize_t read_firmware_revision(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      void *buf, uint16_t len,
                                      uint16_t offset) {
  uint8_t firmware_revision[] = {0, 1, 8};
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &firmware_revision,
                           sizeof(firmware_revision));
}

static ssize_t read_hardware_revision(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      void *buf, uint16_t len,
                                      uint16_t offset) {
  uint32_t hw_rev;
  int rc;

  // Read the hardware revision integer from NVS
  rc = read_hw_revision(&hw_rev);
  if (rc < 0) {
    LOG_ERR("Failed to read hardware revision (err %d)", rc);
    hw_rev = 0; // Default value if read fails
  }

  // Return the raw byte representation
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &hw_rev,
                           sizeof(hw_rev));
}

static ssize_t read_model(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
  char model[] = "BR1";
  return bt_gatt_attr_read(conn, attr, buf, len, offset, model, strlen(model));
}

void update_advertising_data(char adv_name) {
  write_adv_name(adv_name, true);
  sys_reboot(1);
  return;
}

static ssize_t update_ble_name(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr, void *buf,
                               uint16_t len, uint16_t offset) {
  // Check if there's an offset (shouldn't happen for this attribute)
  if (offset > 0) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  }

  // Check if length exceeds expected bounds
  if (len > 31) {
    LOG_ERR("Buffer length exceeds maximum allowed size.");
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }

  // Allocate a local buffer and copy data
  char local_buf[32]; // Size to hold 31 chars plus null terminator
  memcpy(local_buf, buf, len);

  // Ensure null termination
  local_buf[len] = '\0';

  // Log the received string
  LOG_INF("Received string (len=%u): %s", len, local_buf);

  // Process the data
  write_adv_name(local_buf, true);

  // Return the number of bytes read
  return len;
}

BT_GATT_SERVICE_DEFINE(
    gpio_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_AUTOMATION_IO),
    BT_GATT_CHARACTERISTIC(BT_UUID_DIGITAL,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_relay,
                           set_relay, NULL),
    BT_GATT_PRIMARY_SERVICE(BT_UUID_BATTERY),
    BT_GATT_CHARACTERISTIC(BT_UUID_BATTERY_LEVEL, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ, read_battery_level, NULL, NULL),
    BT_GATT_PRIMARY_SERVICE(BT_UUID_CUSTOM_DEVICE_NAME),
    BT_GATT_CHARACTERISTIC(BT_UUID_CUSTOM_DEVICE_NAME, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, update_ble_name, NULL),
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DEVICE_INFO),
    BT_GATT_CHARACTERISTIC(BT_UUID_FIRMWARE_REVISION, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ, read_firmware_revision, NULL,
                           NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_HARDWARE_REVISION, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ, read_hardware_revision, NULL,
                           NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_MODEL_NUMBER, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ, read_model, NULL, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_PRESENTATION_FORMAT, BT_GATT_PERM_READ, NULL,
                       NULL, voltage_level_cpf));

static void connected(struct bt_conn *conn, uint8_t err) {
  if (err) {
    LOG_INF("Connection failed (err %u)\n", err);
    return;
  }
  // Suggest a connection parameter update // not needed if
  // CONFIG_BT_GAP_AUTO_UPDATE_CONN_PARAMS is set in config
  /* 	int update_err = bt_conn_le_param_update(conn, conn_params);
          if (update_err)
          {
                  printk("Connection parameter update failed (err %d)\n",
     update_err);
          } */
  status_led_update(CONNECTED);
  LOG_INF("Connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
  status_led_update(ADVERTISING);
  LOG_INF("Disconnected (reason %u)\n", reason);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err) {
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

  if (!err) {
    LOG_INF("Security changed: %s level %u\n", addr, level);
  } else {
    LOG_INF("Security failed: %s level %u err %d\n", addr, level, err);
  }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey) {
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

  LOG_INF("Passkey for %s: %06u\n", addr, passkey);
}

static void auth_cancel(struct bt_conn *conn) {
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

  LOG_INF("Pairing cancelled: %s\n", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded) {
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

  LOG_INF("Pairing completed: %s, bonded: %d\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

  LOG_INF("Pairing failed conn: %s, reason %d\n", addr, reason);
}

static void pairing_confirm(struct bt_conn *conn) {
  LOG_INF("Accepting pairing request");
  bt_conn_auth_pairing_confirm(conn);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .passkey_display = NULL, // auth_passkey_display,
    .pairing_confirm = pairing_confirm,
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
    .pairing_complete = pairing_complete, .pairing_failed = pairing_failed};

static bool bms_authorize(struct bt_conn *conn,
                          struct bt_bms_authorize_params *params) {
  if ((params->code_len == sizeof(bms_auth_code)) &&
      (memcmp(bms_auth_code, params->code, sizeof(bms_auth_code)) == 0)) {
    LOG_INF("Authorization of BMS operation is successful\n");
    return true;
  }

  LOG_INF("Authorization of BMS operation has failed\n");
  return false;
}

static struct bt_bms_cb bms_callbacks = {
    .authorize = bms_authorize,
};

static int bms_init(void) {
  struct bt_bms_init_params init_params = {0};

  /* Enable all possible operation codes */
  init_params.features.delete_requesting.supported = true;
  init_params.features.delete_rest.supported = true;
  init_params.features.delete_all.supported = true;

  /* Require authorization code for operations that
   * also delete bonding information for other devices
   * than the requesting client.
   */
  init_params.features.delete_rest.authorize = true;
  init_params.features.delete_all.authorize = true;

  init_params.cbs = &bms_callbacks;

  return bt_bms_init(&init_params);
}

static char bt_name[32];

static int get_saved_bt_name(const char *name, size_t len,
                             settings_read_cb read_cb, void *cb_arg) {
  if (strcmp(name, "bt_name") == 0) {
    if (len > sizeof(bt_name) - 1) {
      return -ENOMEM; // Error: buffer too small
    }
    ssize_t ret = read_cb(cb_arg, bt_name, len);
    if (ret > 0) {
      bt_name[ret] = '\0'; // Null-terminate the string
    }
    return 0;
  }
  return -ENOENT; // Key not found
}

struct settings_handler settings_conf = {.name = "bt_name",
                                         .h_set = get_saved_bt_name};

void ble_start() {
  LOG_INF("Starting Bluetooth Peripheral BMS example\n");
  int err;

  err = bt_conn_auth_cb_register(&conn_auth_callbacks);
  if (err) {
    LOG_INF("Failed to register authorization callbacks.\n");
    return;
  }

  err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
  if (err) {
    LOG_INF("Failed to register authorization info callbacks.\n");
    return;
  }

  err = bt_enable(NULL);
  if (err) {
    LOG_INF("Bluetooth init failed (err %d)\n", err);
    return;
  }

  LOG_INF("Bluetooth initialized\n");

  if (IS_ENABLED(CONFIG_SETTINGS)) {
    settings_load();
  }

  struct bt_le_adv_param adv_params = *(BT_LE_ADV_CONN);
  adv_params.interval_max = 2400;
  adv_params.interval_min = 2400;

  struct bt_data ad[2] = {
      BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR))};

  char *name = read_adv_name();
  if (!name) {
    name = DEVICE_NAME;
  }
  LOG_INF("Name: %s\n", name);
  struct bt_data data = BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name));
  ad[1] = data;

  err = bt_le_adv_start(&adv_params, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
  if (err) {
    LOG_ERR("Advertising failed to start (err %d)\n", err);
    return;
  };
  status_led_start(ADVERTISING);
  LOG_INF("Advertising successfully started\n");
}