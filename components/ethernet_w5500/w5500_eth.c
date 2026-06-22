#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "w5500_eth.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "sdkconfig.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_event.h"
#include <stdbool.h>

static const char *TAG = "w5500_eth";
static esp_netif_t *s_eth_netif = NULL;
static bool s_eth_driver_ready = false;
static bool s_eth_link_up = false;
#if CONFIG_ETH_SPI_ETHERNET_W5500
static eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
static esp_event_handler_instance_t s_eth_ev_inst = NULL;
static esp_event_handler_instance_t s_ip_ev_inst = NULL;
static esp_eth_handle_t s_eth_handle = NULL;

/* Ethernet and IP event handler: logs Ethernet state changes and IP acquisition */
static void eth_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == ETH_EVENT) {
        switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            s_eth_link_up = true;
            ESP_LOGI(TAG, "ETH_EVENT: CONNECTED");
            if (s_eth_handle) {
                uint8_t mac[6] = {0};
                if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK) {
                    ESP_LOGI(TAG, "ETH MAC after CONNECTED: %02x:%02x:%02x:%02x:%02x:%02x",
                             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                } else {
                    ESP_LOGW(TAG, "Failed to get MAC via esp_eth_ioctl after CONNECTED");
                }
            }
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            s_eth_link_up = false;
            ESP_LOGI(TAG, "ETH_EVENT: DISCONNECTED");
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "ETH_EVENT: START");
            break;
        case ETHERNET_EVENT_STOP:
            s_eth_link_up = false;
            ESP_LOGI(TAG, "ETH_EVENT: STOP");
            break;
        default:
            ESP_LOGI(TAG, "ETH_EVENT: id=%" PRId32, event_id);
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ipstr[32];
        char nmstr[32];
        char gwstr[32];
        snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(nmstr, sizeof(nmstr), IPSTR, IP2STR(&event->ip_info.netmask));
        snprintf(gwstr, sizeof(gwstr), IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG, "IP_EVENT_ETH_GOT_IP: IP=%s NETMASK=%s GW=%s", ipstr, nmstr, gwstr);
    }
}
#endif

bool w5500_eth_driver_ready(void)
{
    return s_eth_driver_ready;
}

bool w5500_eth_link_up(void)
{
#if CONFIG_ETH_SPI_ETHERNET_W5500
    return s_eth_link_up;
#else
    return false;
#endif
}

esp_err_t w5500_eth_init(bool use_static)
{

                    static bool isr_installed = false;
                if (!isr_installed) {
                    ESP_ERROR_CHECK(gpio_install_isr_service(0));
                    isr_installed = true;
                }

    esp_err_t ret;
    ESP_LOGI(TAG, "ETH INIT START");

    /* Initialize SPI2_HOST for W5500 */
    spi_bus_config_t buscfg = {
        .mosi_io_num = 13,
        .miso_io_num = 14,
        .sclk_io_num = 15,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1520,
    };

    ESP_LOGI(TAG, "Initializing SPI2_HOST: MOSI=%d MISO=%d SCLK=%d max_transfer=%d", 13, 14, 15, 1520);

    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %d", ret);
        s_eth_driver_ready = false;
        return ret;
    }

    /* Configure/reset pins
     * RST: GPIO39
     * INT: GPIO12 (input)
     * CS handled by SPI device or external wiring - still ensure pin exists if used
     */
    gpio_reset_pin(GPIO_NUM_39);
    gpio_set_direction(GPIO_NUM_39, GPIO_MODE_OUTPUT);
    /* Reset sequence: LOW 100ms, HIGH, wait 300ms */
    gpio_set_level(GPIO_NUM_39, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GPIO_NUM_39, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

#if CONFIG_ETH_SPI_ETHERNET_W5500
    /* Initialize W5500 MAC using ESP-IDF 5.5.1 API (MAC-only device over SPI) */
    esp_eth_mac_t *mac = NULL;
    esp_eth_handle_t eth_handle = NULL;

    /* SPI device config for W5500 (CS pin handled here) */
    spi_device_interface_config_t spi_devcfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,
        .duty_cycle_pos = 0,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = 10000000,
        .input_delay_ns = 0,
        .spics_io_num = GPIO_NUM_16,
        .queue_size = 20,
        .flags = 0,
        .pre_cb = NULL,
        .post_cb = NULL
    };

    ESP_LOGI(TAG, "W5500 SPI device config: mode=%d clock_hz=%" PRIu32 " spics=%d",
             spi_devcfg.mode, (uint32_t)spi_devcfg.clock_speed_hz, spi_devcfg.spics_io_num);

    /* W5500 MAC-specific config using provided macro */
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);
    w5500_cfg.int_gpio_num = GPIO_NUM_12;

    /* Use existing mac_config (eth_mac_config_t) declared above */
    mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_w5500 returned NULL");
        return ESP_FAIL;
    }

    /* Create PHY config for W5500 (PHY is required by esp_eth driver) */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    /* Use the same reset pin used earlier so PHY reset is coherent */
    phy_config.reset_gpio_num = GPIO_NUM_39;
    phy_config.phy_addr = 0;

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_w5500 returned NULL");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_config, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %d", ret);
        s_eth_driver_ready = false;
        return ret;
    }

    /* Save handle for event handler access */
    s_eth_handle = eth_handle;

    /* Ensure driver has a valid MAC: read base MAC from efuse and assign to driver */
    {
        uint8_t base_mac[6] = {0};
        uint8_t mac_addr[6] = {0};
        esp_err_t r = esp_read_mac(base_mac, ESP_MAC_ETH);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "esp_read_mac(ESP_MAC_ETH) failed: %d, trying WIFI_STA", r);
            if (esp_read_mac(base_mac, ESP_MAC_WIFI_STA) != ESP_OK) {
                ESP_LOGW(TAG, "esp_read_mac fallback also failed");
            }
        }
        /* Derive a local MAC from base (ensures unicast/local) */
        esp_derive_local_mac(mac_addr, base_mac);

        r = esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "ETH MAC assigned to driver: %02x:%02x:%02x:%02x:%02x:%02x",
                     mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        } else {
            ESP_LOGW(TAG, "Failed to set MAC via esp_eth_ioctl: %d", r);
        }
        /* Verify by querying the driver */
        uint8_t got[6] = {0};
        if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, got) == ESP_OK) {
            ESP_LOGI(TAG, "ETH MAC after driver_install: %02x:%02x:%02x:%02x:%02x:%02x",
                     got[0], got[1], got[2], got[3], got[4], got[5]);
        }
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return ESP_FAIL;
    }

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    if (glue == NULL) {
        ESP_LOGE(TAG, "esp_eth_new_netif_glue failed (NULL)");
        return ESP_FAIL;
    }

    ret = esp_netif_attach(s_eth_netif, glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %d", ret);
        return ret;
    }

    /* Print esp-netif MAC as seen by the netif */
    {
        uint8_t netif_mac[6] = {0};
        if (esp_netif_get_mac(s_eth_netif, netif_mac) == ESP_OK) {
            ESP_LOGI(TAG, "esp-netif MAC after attach: %02x:%02x:%02x:%02x:%02x:%02x",
                     netif_mac[0], netif_mac[1], netif_mac[2], netif_mac[3], netif_mac[4], netif_mac[5]);
        } else {
            ESP_LOGW(TAG, "esp_netif_get_mac failed after attach");
        }
    }

    /* Register handlers to log Ethernet and IP events BEFORE starting driver */
    ret = esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                             &eth_ip_event_handler, NULL, &s_eth_ev_inst);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register ETH_EVENT handler: %d", ret);
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &eth_ip_event_handler, NULL, &s_ip_ev_inst);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register IP_EVENT_ETH_GOT_IP handler: %d", ret);
    }

    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %d", ret);
        s_eth_driver_ready = false;
        return ret;
    }

    /* Start DHCP client on Ethernet netif if not using static IP */
    if (!use_static) {
        ret = esp_netif_dhcpc_start(s_eth_netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "esp_netif_dhcpc_start failed: %d", ret);
        } else {
            ESP_LOGI(TAG, "DHCP client started on Ethernet netif");
        }
    } else {
        ESP_LOGI(TAG, "User configured static IP for Ethernet; DHCP not started");
    }

    ESP_LOGI(TAG, "ETH STARTED");
    ESP_LOGI(TAG, "WAITING FOR IP");
    s_eth_driver_ready = true;
    return ESP_OK;
#else
    ESP_LOGW(TAG, "W5500 driver not enabled in sdkconfig; skipping esp_eth initialization");
    s_eth_driver_ready = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_netif_t *w5500_eth_get_netif(void)
{
    return s_eth_netif;
}
