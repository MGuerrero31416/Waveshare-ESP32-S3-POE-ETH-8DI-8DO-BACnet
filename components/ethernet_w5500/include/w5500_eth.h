#ifndef W5500_ETH_H
#define W5500_ETH_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

/**
 * Initialize W5500-based Ethernet over SPI (SPI2_HOST).
 * Configures SPI pins, performs hardware reset, installs driver and starts the interface.
 * Returns ESP_OK on success or an error code. If the IDF W5500 driver is not enabled
 * at build time this will return ESP_ERR_NOT_SUPPORTED.
 */
/*
 * Initialize W5500-based Ethernet over SPI (SPI2_HOST).
 * @param use_static: true to use static IP (do not start DHCP), false to start DHCP client
 */
esp_err_t w5500_eth_init(bool use_static);

/**
 * Return true once the W5500 Ethernet driver has been installed and started successfully.
 */
bool w5500_eth_driver_ready(void);

/**
 * Return current physical link status reported by the ETH driver.
 * True means link up, false means link down or status unavailable.
 */
bool w5500_eth_link_up(void);

/**
 * Return the esp-netif handle created for Ethernet, or NULL if not initialized.
 */
esp_netif_t *w5500_eth_get_netif(void);

#endif // W5500_ETH_H
