#pragma once

#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

#include <stdbool.h>

#define BLE_CONFIG_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CONFIG_CHAR_UUID    "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

/* Provisioning service and characteristic 16-bit UUIDs (custom/vendor range) */
#define PROV_SERVICE_UUID16 0xFFF0
#define PROV_CHAR_SSID_UUID16 0xFFF1
#define PROV_CHAR_PASS_UUID16 0xFFF2
#define PROV_CHAR_NETMODE_UUID16 0xFFF3
#define PROV_CHAR_STATICIP_UUID16 0xFFF4
#define PROV_CHAR_APPLY_UUID16 0xFFF5

void ble_config_init(void);

#endif // BLE_CONFIG_H
