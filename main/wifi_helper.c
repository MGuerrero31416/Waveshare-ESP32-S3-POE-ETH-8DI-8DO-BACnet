#include "wifi_helper.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "bacnet_network.h"
#include "User_Settings.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include <string.h>

static const char *TAG = "wifi_helper";
static EventGroupHandle_t s_wifi_event_group;
static bool s_ip_logged;
static bool s_disconnect_logged;
static bool s_wifi_initialized;
static bool s_wifi_started;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;

void bacnet_bip_network_ready(void);

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_CFG_NAMESPACE "wifi_cfg"

static bool wifi_has_text(const char *text)
{
    return text && text[0] != '\0';
}

static bool wifi_load_credentials_nvs(char *ssid, size_t ssid_len,
                                      char *password, size_t password_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t ssid_size = ssid_len;
    size_t pass_size = password_len;
    err = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    if (err != ESP_OK || ssid_size <= 1) {
        nvs_close(handle);
        return false;
    }

    err = nvs_get_str(handle, "pass", password, &pass_size);
    if (err != ESP_OK) {
        password[0] = '\0';
    }

    nvs_close(handle);
    return true;
}

esp_err_t wifi_save_credentials_nvs(const char *ssid, const char *password)
{
    if (!wifi_has_text(ssid)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "pass", password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

bool app_wifi_is_initialized(void)
{
    return s_wifi_initialized;
}

bool app_wifi_is_started(void)
{
    return s_wifi_started;
}

bool app_wifi_is_connected(void)
{
    wifi_ap_record_t ap_info = {0};

    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip_logged = false;
        if (!s_disconnect_logged) {
            ESP_LOGI(TAG, "WiFi disconnected");
            s_disconnect_logged = true;
        }
        esp_err_t reconnect_err = esp_wifi_connect();
        if (reconnect_err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi reconnect requested");
        } else {
            ESP_LOGW(TAG, "WiFi reconnect failed: %s", esp_err_to_name(reconnect_err));
        }
        bacnet_network_notify(BACNET_NETWORK_EVT_WIFI_LOST_IP);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (event && !s_ip_logged) {
            ESP_LOGD(TAG, "WiFi connected with IP: " IPSTR, IP2STR(&event->ip_info.ip));
            wifi_config_t cfg = { 0 };
            if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK &&
                wifi_has_text((const char *)cfg.sta.ssid)) {
                ESP_LOGD(TAG, "WiFi connected to SSID: %s", (const char *)cfg.sta.ssid);
            } else {
                ESP_LOGD(TAG, "WiFi connected (SSID unknown)");
            }
            s_ip_logged = true;
            s_disconnect_logged = false;
        }
        esp_wifi_set_ps(WIFI_PS_NONE);   // disable modem sleep – prevents ping timeouts
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        bacnet_network_notify(BACNET_NETWORK_EVT_WIFI_GOT_IP);
    }
}

/* Simple Wi-Fi setup (blocking until connected) */
void wifi_init_sta(void)
{
    if (s_wifi_initialized) {
        ESP_LOGW(TAG, "wifi_init_sta already initialized; reusing existing driver state");
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect retry failed: %s", esp_err_to_name(err));
        }
        return;
    }

    /* Initialize networking stack (only call once in app_main context) */
    /* esp_netif_init() and esp_event_loop_create_default() should be called before this */
    s_wifi_event_group = xEventGroupCreate();
    s_ip_logged = false;
    s_disconnect_logged = false;
    s_wifi_started = false;
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return;
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_t *wifi_sta = esp_netif_create_default_wifi_sta();
        if (wifi_sta == NULL) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta returned NULL");
            return;
        }
        ESP_LOGI(TAG, "esp_netif_create_default_wifi_sta succeeded");
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    ESP_LOGI(TAG, "esp_wifi_init returned %d (%s)", (int)err, esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return;
    }
    // Keep credentials in flash so device can use provisioned Wi-Fi after reboot.
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(err));
    }

    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &wifi_event_handler,
                                              NULL,
                                              &s_wifi_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler,
                                              NULL,
                                              &s_ip_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "WiFi event hooks registered (WIFI_EVENT, IP_EVENT_STA_GOT_IP)");

    esp_netif_t *esp_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (USER_WIFI_USE_STATIC_IP && esp_netif) {
        err = esp_netif_dhcpc_stop(esp_netif);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_dhcpc_stop failed: %s", esp_err_to_name(err));
        }
        esp_netif_ip_info_t ip_info = {0};
        ip4_addr_t ip4 = {0};
        if (ip4addr_aton(USER_WIFI_STATIC_IP_ADDR, &ip4)) {
            ip_info.ip.addr = ip4.addr;
        }
        if (ip4addr_aton(USER_WIFI_STATIC_IP_GATEWAY, &ip4)) {
            ip_info.gw.addr = ip4.addr;
        }
        if (ip4addr_aton(USER_WIFI_STATIC_IP_NETMASK, &ip4)) {
            ip_info.netmask.addr = ip4.addr;
        }
        err = esp_netif_set_ip_info(esp_netif, &ip_info);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_set_ip_info failed: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Using static IP %s", USER_WIFI_STATIC_IP_ADDR);
    }

    char ssid[33] = {0};
    char password[65] = {0};
    bool have_credentials = false;

    if (wifi_load_credentials_nvs(ssid, sizeof(ssid), password, sizeof(password))) {
        have_credentials = true;
        ESP_LOGI(TAG, "Using Wi-Fi credentials from NVS namespace '%s'", WIFI_CFG_NAMESPACE);
    } else if (wifi_has_text(USER_WIFI_SSID)) {
        strncpy(ssid, USER_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(password, USER_WIFI_PASS, sizeof(password) - 1);
        have_credentials = true;
        ESP_LOGI(TAG, "Using compile-time Wi-Fi defaults");
    } else {
        ESP_LOGI(TAG, "No app Wi-Fi defaults set; trying provisioned credentials in Wi-Fi flash storage");
    }

    wifi_config_t wifi_config = { 0 };
    if (have_credentials) {
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        if (!wifi_has_text(password)) {
            // Only permit open auth when intentionally configured with empty password.
            wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        }
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "esp_wifi_set_mode succeeded: WIFI_MODE_STA");
    if (have_credentials) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "esp_wifi_set_config succeeded: WIFI_IF_STA");
    }
    err = esp_wifi_start();
    ESP_LOGI(TAG, "esp_wifi_start returned %d (%s)", (int)err, esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return;
    }
    s_wifi_initialized = true;
    s_wifi_started = true;
    err = esp_wifi_set_ps(WIFI_PS_NONE);   // disable modem sleep before connecting
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps failed: %s", esp_err_to_name(err));
    }

    if (have_credentials) {
        ESP_LOGI(TAG, "Connecting to Wi-Fi %s ...", ssid);
    } else {
        ESP_LOGI(TAG, "Connecting to provisioned Wi-Fi credentials ...");
    }
    err = esp_wifi_connect();
    ESP_LOGI(TAG, "esp_wifi_connect returned %d (%s)", (int)err, esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }

    EventBits_t bits = 0;
    if (s_wifi_event_group) {
        bits = xEventGroupWaitBits(s_wifi_event_group,
                                   WIFI_CONNECTED_BIT,
                                   pdFALSE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(10000));
    }

    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "WiFi connection timeout - proceeding anyway");
    }
}
