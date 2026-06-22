#pragma once

/* Minimal app-facing ble header to avoid build-order include issues.
 * The full API and UUIDs live in the component header at
 * components/ble_config/include/ble_config.h
 */
void ble_config_init(void);
