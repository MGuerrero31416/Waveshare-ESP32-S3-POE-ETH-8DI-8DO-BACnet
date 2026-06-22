#include "ble_config.h"
#include "User_Settings.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "cJSON.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"

static const char *TAG = "BLE_CONFIG";

static uint16_t gatts_if_local = 0;
static int service_handle = 0;
static int char_handle = 0;

/* Provisioning service handles and storage */
static uint16_t prov_service_handle = 0;
static uint16_t prov_char_handle_ssid = 0;
static uint16_t prov_char_handle_pass = 0;
static uint16_t prov_char_handle_netmode = 0;
static uint16_t prov_char_handle_staticip = 0;
static uint16_t prov_char_handle_apply = 0;
static bool prov_create_pending = false;
static int prov_chars_expected = 0;

static char prov_ssid[33] = {0};
static char prov_pass[65] = {0};
static char prov_static_ip[40] = {0};
static uint8_t prov_netmode = 0; /* 0 DHCP, 1 Static */

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Advertising started successfully");
            } else {
                ESP_LOGE(TAG, "Advertising start failed: %d", param->adv_start_cmpl.status);
            }
            break;
        default:
            break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
        case ESP_GATTS_REG_EVT:
            ESP_LOGI(TAG, "GATTS Registered, app_id=%d", param->reg.app_id);
            gatts_if_local = gatts_if;
            /* Create provisioning service (16-bit vendor UUIDs) */
            {
                esp_gatt_srvc_id_t service_id = {0};
                service_id.is_primary = true;
                service_id.id.inst_id = 0;
                service_id.id.uuid.len = ESP_UUID_LEN_16;
                service_id.id.uuid.uuid.uuid16 = PROV_SERVICE_UUID16;
                esp_err_t r = esp_ble_gatts_create_service(gatts_if, &service_id, 6);
                if (r == ESP_OK) {
                    prov_create_pending = true;
                    prov_chars_expected = 5;
                    ESP_LOGI(TAG, "Provisioning service creation requested");
                } else {
                    ESP_LOGE(TAG, "Failed to request provisioning service create: %d", r);
                }
            }
            break;
        case ESP_GATTS_CREATE_EVT:
            ESP_LOGI(TAG, "Service created, status=%d, service_handle=%d", param->create.status, param->create.service_handle);
            service_handle = param->create.service_handle;
            if (prov_create_pending) {
                prov_service_handle = param->create.service_handle;
                prov_create_pending = false;
                /* Add provisioning characteristics: SSID, PASS, NETMODE, STATIC_IP, APPLY */
                {
                    esp_bt_uuid_t char_uuid = {0};
                    /* SSID */
                    char_uuid.len = ESP_UUID_LEN_16;
                    char_uuid.uuid.uuid16 = PROV_CHAR_SSID_UUID16;
                    esp_ble_gatts_add_char(prov_service_handle, &char_uuid,
                        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                        ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                        NULL, NULL);
                    /* PASS */
                    char_uuid.uuid.uuid16 = PROV_CHAR_PASS_UUID16;
                    esp_ble_gatts_add_char(prov_service_handle, &char_uuid,
                        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                        ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                        NULL, NULL);
                    /* NETMODE */
                    char_uuid.uuid.uuid16 = PROV_CHAR_NETMODE_UUID16;
                    esp_ble_gatts_add_char(prov_service_handle, &char_uuid,
                        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                        ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                        NULL, NULL);
                    /* STATIC IP */
                    char_uuid.uuid.uuid16 = PROV_CHAR_STATICIP_UUID16;
                    esp_ble_gatts_add_char(prov_service_handle, &char_uuid,
                        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                        ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                        NULL, NULL);
                    /* APPLY */
                    char_uuid.uuid.uuid16 = PROV_CHAR_APPLY_UUID16;
                    esp_ble_gatts_add_char(prov_service_handle, &char_uuid,
                        ESP_GATT_PERM_WRITE,
                        ESP_GATT_CHAR_PROP_BIT_WRITE,
                        NULL, NULL);
                }
                ESP_LOGI(TAG, "Provisioning service created (handle=%d), chars requested", prov_service_handle);
            }
            break;
        case ESP_GATTS_ADD_CHAR_EVT:
            ESP_LOGI(TAG, "Char added, attr_handle=%d", param->add_char.attr_handle);
            if (prov_chars_expected > 0 && prov_service_handle && param->add_char.attr_handle) {
                /* assign to next provisioning char */
                if (!prov_char_handle_ssid) {
                    prov_char_handle_ssid = param->add_char.attr_handle;
                } else if (!prov_char_handle_pass) {
                    prov_char_handle_pass = param->add_char.attr_handle;
                } else if (!prov_char_handle_netmode) {
                    prov_char_handle_netmode = param->add_char.attr_handle;
                } else if (!prov_char_handle_staticip) {
                    prov_char_handle_staticip = param->add_char.attr_handle;
                } else if (!prov_char_handle_apply) {
                    prov_char_handle_apply = param->add_char.attr_handle;
                }
                prov_chars_expected--;
                if (prov_chars_expected == 0) {
                    ESP_LOGI(TAG, "All provisioning characteristics added: ssid=%d pass=%d netmode=%d staticip=%d apply=%d",
                        prov_char_handle_ssid, prov_char_handle_pass, prov_char_handle_netmode, prov_char_handle_staticip, prov_char_handle_apply);
                }
            } else {
                char_handle = param->add_char.attr_handle;
            }
            break;
        case ESP_GATTS_READ_EVT: {
            ESP_LOGI(TAG, "Read request received");
            // build JSON
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "device_name", USER_BACNET_DEVICE_NAME);
            cJSON_AddNumberToObject(root, "device_instance", USER_BACNET_DEVICE_INSTANCE);
            cJSON_AddBoolToObject(root, "wifi_enabled", USER_ENABLE_BACNET_IP_WIFI);
            cJSON_AddBoolToObject(root, "ethernet_enabled", USER_ENABLE_BACNET_IP_ETHERNET);
            cJSON_AddBoolToObject(root, "mstp_enabled", USER_ENABLE_BACNET_MSTP);
            char *json = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, NULL);
            esp_ble_gatts_set_attr_value(char_handle, strlen(json), (const uint8_t*)json);
            ESP_LOGI(TAG, "Provided config JSON: %s", json);
            free(json);
            break;
        }
        case ESP_GATTS_WRITE_EVT: {
            ESP_LOGI(TAG, "Write event, len=%d handle=%d", param->write.len, param->write.handle);
            /* Provisioning characteristics handling */
            if (param->write.handle == prov_char_handle_ssid) {
                size_t len = param->write.len < sizeof(prov_ssid)-1 ? param->write.len : sizeof(prov_ssid)-1;
                memcpy(prov_ssid, param->write.value, len);
                prov_ssid[len] = '\0';
                ESP_LOGI(TAG, "Provisioning SSID written: %s", prov_ssid);
                nvs_handle_t h;
                if (nvs_open("ble_prov", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_str(h, "ssid", prov_ssid);
                    nvs_commit(h);
                    nvs_close(h);
                }
            } else if (param->write.handle == prov_char_handle_pass) {
                size_t len = param->write.len < sizeof(prov_pass)-1 ? param->write.len : sizeof(prov_pass)-1;
                memcpy(prov_pass, param->write.value, len);
                prov_pass[len] = '\0';
                ESP_LOGI(TAG, "Provisioning PASS written (len=%d)", (int)len);
                nvs_handle_t h;
                if (nvs_open("ble_prov", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_str(h, "pass", prov_pass);
                    nvs_commit(h);
                    nvs_close(h);
                }
            } else if (param->write.handle == prov_char_handle_netmode) {
                if (param->write.len >= 1) {
                    prov_netmode = param->write.value[0];
                    ESP_LOGI(TAG, "Provisioning netmode written: %u", (unsigned)prov_netmode);
                    nvs_handle_t h;
                    if (nvs_open("ble_prov", NVS_READWRITE, &h) == ESP_OK) {
                        nvs_set_u8(h, "netmode", prov_netmode);
                        nvs_commit(h);
                        nvs_close(h);
                    }
                }
            } else if (param->write.handle == prov_char_handle_staticip) {
                size_t len = param->write.len < sizeof(prov_static_ip)-1 ? param->write.len : sizeof(prov_static_ip)-1;
                memcpy(prov_static_ip, param->write.value, len);
                prov_static_ip[len] = '\0';
                ESP_LOGI(TAG, "Provisioning static IP written: %s", prov_static_ip);
                nvs_handle_t h;
                if (nvs_open("ble_prov", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_str(h, "static_ip", prov_static_ip);
                    nvs_commit(h);
                    nvs_close(h);
                }
            } else if (param->write.handle == prov_char_handle_apply) {
                ESP_LOGI(TAG, "Provisioning APPLY written: committing and restarting");
                /* Values are committed per-handle on writes; restart now to apply */
                esp_restart();
            } else {
                /* Existing JSON-based write handler: parse JSON payload and log values */
                ESP_LOGI(TAG, "(Fallback) JSON write handler invoked, len=%d", param->write.len);
                char *buf = malloc(param->write.len + 1);
                memcpy(buf, param->write.value, param->write.len);
                buf[param->write.len] = '\0';
                cJSON *root = cJSON_Parse(buf);
                if (root) {
                    cJSON *name = cJSON_GetObjectItem(root, "device_name");
                    cJSON *instance = cJSON_GetObjectItem(root, "device_instance");
                    cJSON *wifi = cJSON_GetObjectItem(root, "wifi_enabled");
                    cJSON *eth = cJSON_GetObjectItem(root, "ethernet_enabled");
                    cJSON *mstp = cJSON_GetObjectItem(root, "mstp_enabled");
                    if (name && cJSON_IsString(name)) {
                        ESP_LOGI(TAG, "Received device_name: %s", name->valuestring);
                    }
                    if (instance && cJSON_IsNumber(instance)) {
                        ESP_LOGI(TAG, "Received device_instance: %d", instance->valueint);
                    }
                    if (wifi && cJSON_IsBool(wifi)) {
                        ESP_LOGI(TAG, "Received wifi_enabled: %s", cJSON_IsTrue(wifi) ? "true" : "false");
                    }
                    if (eth && cJSON_IsBool(eth)) {
                        ESP_LOGI(TAG, "Received ethernet_enabled: %s", cJSON_IsTrue(eth) ? "true" : "false");
                    }
                    if (mstp && cJSON_IsBool(mstp)) {
                        ESP_LOGI(TAG, "Received mstp_enabled: %s", cJSON_IsTrue(mstp) ? "true" : "false");
                    }
                    cJSON_Delete(root);
                } else {
                    ESP_LOGE(TAG, "Failed to parse JSON from write");
                }
                free(buf);
            }
            break;
        }
        default:
            break;
    }
}

void ble_config_init(void) {
    if (!USER_ENABLE_BLE_CONFIG) {
        ESP_LOGI(TAG, "BLE config disabled by USER_ENABLE_BLE_CONFIG");
        return;
    }

    esp_err_t ret;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %d", ret);
        return;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %d", ret);
        return;
    }
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "esp_bluedroid_init failed: %d", ret);
        return;
    }
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed: %d", ret);
        return;
    }

    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gatts_app_register(0);

    // advertise
    const char *adv_name_fmt = "BACnet-ESP32-%d";
    char adv_name[32];
    snprintf(adv_name, sizeof(adv_name), adv_name_fmt, USER_BACNET_DEVICE_INSTANCE);
    esp_ble_gap_set_device_name(adv_name);

    esp_ble_adv_data_t adv_data = {0};
    adv_data.set_scan_rsp = false;
    adv_data.include_name = true;
    adv_data.include_txpower = false;
    adv_data.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    esp_ble_gap_config_adv_data(&adv_data);

    esp_ble_adv_params_t adv_params = {0};
    adv_params.adv_int_min = 0x20;
    adv_params.adv_int_max = 0x40;
    adv_params.adv_type = ADV_TYPE_IND;
    adv_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    adv_params.channel_map = ADV_CHNL_ALL;
    adv_params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    esp_ble_gap_start_advertising(&adv_params);

    ESP_LOGI(TAG, "BLE config initialized and advertising as %s", adv_name);
}
