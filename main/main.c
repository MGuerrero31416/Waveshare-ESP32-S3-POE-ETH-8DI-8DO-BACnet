/* Minimal example: connect to Wi‑Fi and initialize BACnet. */
#include "driver/gpio.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "wifi_helper.h"
#include "bacnet_network.h"
#include "bacnet_system_fsm.h"
#include "waveshare_io.h"
#include "ble_config.h"
#include "analog_value.h"
#include "binary_value.h"
#include "analog_input.h"
#include "binary_input.h"
#include "binary_output.h"
#include "mstp_rs485.h"
#include "User_Settings.h"
#include "bacnet_io_link.h"
#include "w5500_eth.h"

/* BACnet stack headers */
#include "bacnet/config.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/service/h_apdu.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/datalink/bip.h"
/* BACnet object helpers */
#include "bacnet/basic/object/av.h"
#include "bacnet/basic/object/bv.h"
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/bi.h"
#include "bacnet/basic/object/bo.h"

/* MSTP types */
#include "bacnet/datalink/mstp.h"

#include <stdbool.h>
#include <stdint.h>

#if USER_ENABLE_RGB_STATUS
#include "rgb_status.h"
#endif

#include "esp_heap_caps.h"

static const char *TAG = "bacnet";

#if CONFIG_FREERTOS_UNICORE
#define BACNET_TASK_CORE tskNO_AFFINITY
#else
#define BACNET_TASK_CORE 1
#endif

int override_nvs_on_flash = 0;  /* Exported for AV/BV modules */

/* Guard to ensure BACnet/IP is only initialized once */
static bool bacnet_ip_initialized = false;
static bool bacnet_bip_initialized = false;
static bool bacnet_objects_ready = false;
static const char *bacnet_active_ifkey = NULL;
static bool bacnet_mstp_initialized_once = false;

/* Static task storage for BACnet critical tasks to avoid heap fragmentation on startup */
static SemaphoreHandle_t bacnet_datalink_mutex = NULL;
static EventGroupHandle_t bacnet_network_event_group = NULL;
static esp_event_handler_instance_t bacnet_eth_event_instance = NULL;
static esp_event_handler_instance_t bacnet_ip_event_instance = NULL;

/* Static storage for BACnet RTOS objects referenced across modules */
static TaskHandle_t bacnet_network_task_handle = NULL;
static StackType_t bacnet_network_stack[(4096 / sizeof(StackType_t))];
static StaticTask_t bacnet_network_task_buffer;
static TaskHandle_t bacnet_rx_task_handle = NULL;
static StackType_t bacnet_rx_stack[(16384 / sizeof(StackType_t))];
static StaticTask_t bacnet_rx_task_buffer;
static TaskHandle_t bacnet_mstp_task_handle = NULL;

static TaskHandle_t bacnet_io_task_handle = NULL;
static StackType_t bacnet_io_stack[(4096 / sizeof(StackType_t))];
static StaticTask_t bacnet_io_task_buffer;

static TaskHandle_t bacnet_cov_task_handle __attribute__((unused)) = NULL;
static StackType_t bacnet_cov_stack[(8192 / sizeof(StackType_t))] __attribute__((unused));
static StaticTask_t bacnet_cov_task_buffer __attribute__((unused));

/* Forward declarations of tasks used below */
static void bacnet_register_with_bbmd(void);
static void bacnet_bip_transport_ready(void);
static void bacnet_network_task(void *pvParameters);
static esp_err_t bacnet_start_bip_on_interface(const char *ifkey, const char *label);
static void bacnet_start_receive_task(void);
static bool bacnet_mstp_init(void);
static bool bacnet_update_bip_addresses(const char *ifkey, const char *label);
static bool bacnet_is_eth_usable(void);
static bool bacnet_is_wifi_usable(void);
static void bacnet_dump_net_state(const char *context);
static const char *bacnet_selected_transport_name(bool eth_usable, bool wifi_usable);
static void log_heap_state(const char *context);
static bool bacnet_init_objects_once(void);
static bool bacnet_init_mstp_once(void);
static void bacnet_on_transport_active(void);
static void bacnet_log_task_create_request(const char *task_name, unsigned stack_bytes, int core_id);
static void bacnet_log_task_create_result(const char *task_name, TaskHandle_t task_handle, unsigned stack_bytes, int core_id);

/* Forward prototypes for functions used before their definitions */
bool bacnet_datalink_lock(const char *name);
void bacnet_datalink_unlock(void);
static void bacnet_receive_task(void *pvParameters);
static void __attribute__((unused)) bacnet_mstp_receive_task(void *pvParameters);
static void __attribute__((unused)) bacnet_cov_task(void *pvParameters);
void bacnet_network_notify(uint32_t event_bits);
static void bacnet_eth_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data);

/* Forward declarations for datalink name symbols defined later in this file */
static char datalink_bip[];
static char datalink_mstp[];
static char *datalink_default;

/* MSTP counters used in MSTP receive task */
static unsigned long mstp_pdu_count = 0;
static unsigned long mstp_apdu_count = 0;
static unsigned long mstp_rp_total = 0;
static float mstp_rp_last_value = 0.0f;
static unsigned long mstp_wp_total = 0;

static void log_heap_state(const char *context)
{
    ESP_LOGI(TAG,
             "HEAP[%s] free=%u largest=%u min_free=%u",
             context,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

void app_main(void)
{

    ESP_LOGI(TAG, "[BOOT 1/7] Core init: APP_MAIN STARTED");

    /* Core system init: NVS, logging, event loop, network infra */
    esp_err_t ret = nvs_flash_init();

    bacnet_datalink_mutex = xSemaphoreCreateRecursiveMutex();
    if (!bacnet_datalink_mutex) {
        ESP_LOGE(TAG, "Failed to create BACnet datalink mutex");
    }

    override_nvs_on_flash = USER_OVERRIDE_NVS_ON_FLASH;
    if (override_nvs_on_flash && USER_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "OVERRIDE_NVS_ON_FLASH ignored: USER_WIFI_SSID is empty and erase would remove provisioned Wi-Fi credentials");
        override_nvs_on_flash = 0;
    }

    if (override_nvs_on_flash) {
        ESP_LOGI(TAG, "Override flag set - erasing NVS to reset to defaults");
        nvs_flash_erase();
        ret = nvs_flash_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinitialize NVS after erase: %d", ret);
        } else {
            ESP_LOGI(TAG, "NVS reinitialized successfully");
        }
    } else if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "NVS needs initialization");
        nvs_flash_erase();
        nvs_flash_init();
    } else if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized from existing data");
    }

#if USER_ENABLE_RGB_STATUS
    /* Initialize RGB status indicator early (non-blocking) */
    rgb_status_init();
    rgb_status_set_booting();
#endif

    /* Network infra (esp-netif + event loop) */
    ESP_LOGI(TAG, "[BOOT 1/7] Heap snapshot before network infra: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    log_heap_state("boot");
    esp_netif_init();
    esp_event_loop_create_default();
    ESP_LOGI(TAG, "[BOOT 1/7] Core init complete");

    bacnet_fsm_config_t gw_config = {
        .enable_bacnet_ip_ethernet = USER_ENABLE_BACNET_IP_ETHERNET,
        .enable_bacnet_ip_wifi = USER_ENABLE_BACNET_IP_WIFI,
        .enable_bacnet_mstp = USER_ENABLE_BACNET_MSTP
    };
    bacnet_fsm_hooks_t gw_hooks = {
        .init_objects_once    = bacnet_init_objects_once,
        .init_mstp_once       = bacnet_init_mstp_once,
        .is_eth_usable        = bacnet_is_eth_usable,
        .is_wifi_usable       = bacnet_is_wifi_usable,
        .start_bip_on_interface = bacnet_start_bip_on_interface,
        .on_bacnet_active     = bacnet_on_transport_active
    };
    bacnet_fsm_init(&gw_config, &gw_hooks);

    bacnet_network_event_group = xEventGroupCreate();
    if (!bacnet_network_event_group) {
        ESP_LOGE(TAG, "Failed to create BACnet network event group");
    }

    bacnet_network_task_handle = xTaskCreateStaticPinnedToCore(
        bacnet_network_task,
        "bacnet_net",
        (4096 / sizeof(StackType_t)),
        NULL,
        tskIDLE_PRIORITY + 3,
        bacnet_network_stack,
        &bacnet_network_task_buffer,
        BACNET_TASK_CORE);
    if (bacnet_network_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create BACnet network task");
    } else {
        bacnet_log_task_create_result("bacnet_net", bacnet_network_task_handle, 4096, BACNET_TASK_CORE);
    }

    esp_err_t evret = esp_event_handler_instance_register(
        ETH_EVENT,
        ESP_EVENT_ANY_ID,
        &bacnet_eth_event_handler,
        NULL,
        &bacnet_eth_event_instance);
    if (evret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH event handler: %d", evret);
    }

    evret = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_ETH_GOT_IP,
        &bacnet_eth_event_handler,
        NULL,
        &bacnet_ip_event_instance);
    if (evret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH got-IP handler: %d", evret);
    }

    /* Stage 2: Ethernet init */
    ESP_LOGI(TAG, "[BOOT 2/7] Ethernet init starting");
    if (USER_ENABLE_ETHERNET) {
        ESP_LOGI(TAG, "USER_ENABLE_ETHERNET set: initializing W5500 Ethernet");
        /* TEMP HARDWARE RESET TEST (REMOVE AFTER DEBUG) */
        gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
        gpio_set_level(GPIO_NUM_4, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(GPIO_NUM_4, 1);
        ESP_LOGI(TAG, "RESET TEST TOGGLE DONE");

        ESP_LOGI(TAG, ">>> ABOUT TO CALL w5500_eth_init()");
        ESP_LOGI(TAG, "[BOOT 2/7] Heap before w5500_eth_init: free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        if (w5500_eth_init(USER_ETH_USE_STATIC_IP) != ESP_OK) {
            ESP_LOGE(TAG, "W5500 Ethernet init failed");
        } else {
#if USER_ENABLE_RGB_STATUS
            rgb_status_set_waiting_for_ip();
#endif
            ESP_LOGI(TAG, "W5500 Ethernet initialized (waiting for IP)");
        }
    } else {
        ESP_LOGI(TAG, "Ethernet disabled by configuration");
    }
    ESP_LOGI(TAG, "[BOOT 2/7] Ethernet init complete");

    /* Stage 3: BACnet stack will be initialized after Ethernet IP event. Do not init here. */
    ESP_LOGI(TAG, "[BOOT 3/7] BACnet deferred until network-ready (Ethernet IP or WiFi fallback)");

    /* Stage 4: IO expander initialization */
    ESP_LOGI(TAG, "[BOOT 4/7] IO initialization starting");
    ESP_LOGI(TAG, "[BOOT 4/7] Heap before IO init: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    /* Initialize Waveshare IO (TCA9554) early to avoid later contention */
    waveshare_io_init();
    ESP_LOGI(TAG, "[BOOT 4/7] Waveshare IO init complete");
    ESP_LOGI(TAG, "[BOOT 4/7] IO initialization complete");
    /* IO ready – object init will proceed when bacnet_fsm_start() is called below */

    /* Stage 5: BLE initialization (single owner) */
    ESP_LOGI(TAG, "[BOOT 5/7] BLE initialization starting");
    ESP_LOGI(TAG, "[BOOT 5/7] Heap before BLE init: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    log_heap_state("before_ble_init");
    /* Call the single BLE init entry point once */
    ble_config_init();
    ESP_LOGI(TAG, "[BOOT 5/7] Heap after BLE init: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    log_heap_state("after_ble_init");
    ESP_LOGI(TAG, "[BOOT 5/7] BLE initialization complete");
    /* Hardware init complete: start centralized BACnet FSM. */
    bacnet_fsm_start();

    /* Stage 6: WiFi initialization as secondary/fallback */
    ESP_LOGI(TAG, "[BOOT 6/7] WiFi initialization starting");
    ESP_LOGI(TAG, "[BOOT 6/7] Heap before WiFi init: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    log_heap_state("before_wifi_init");
    if (USER_ENABLE_BACNET_IP_WIFI) {
        /* WiFi is always brought up as fallback transport; BACnet/IP startup is task-owned. */
        wifi_init_sta();
        ESP_LOGI(TAG, "[BOOT 6/7] Heap after WiFi init: free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        log_heap_state("after_wifi_init");
    } else {
        ESP_LOGI(TAG, "WiFi disabled by configuration");
    }
    ESP_LOGI(TAG, "[BOOT 6/7] WiFi initialization complete");

    ESP_LOGI(TAG, "[BOOT 7/7] Startup complete – BACnet FSM state=%s",
             bacnet_fsm_state_name(bacnet_fsm_get_state()));

}

static char datalink_bip[] = "bip";
static char datalink_mstp[] = "mstp";
static char *datalink_default = NULL;

static uint8_t mstp_rx_buffer[512];
static uint8_t mstp_tx_buffer[512];
static struct mstp_port_struct_t mstp_port;
static struct dlmstp_user_data_t mstp_user;
static struct dlmstp_rs485_driver mstp_rs485_driver = {
    .init = MSTP_RS485_Init,
    .send = MSTP_RS485_Send,
    .read = MSTP_RS485_Read,
    .transmitting = MSTP_RS485_Transmitting,
    .baud_rate = MSTP_RS485_Baud_Rate,
    .baud_rate_set = MSTP_RS485_Baud_Rate_Set,
    .silence_milliseconds = MSTP_RS485_Silence_Milliseconds,
    .silence_reset = MSTP_RS485_Silence_Reset
};

bool bacnet_datalink_lock(const char *name)
{
    if (xPortInIsrContext() || !bacnet_datalink_mutex) {
        return false;
    }

    if (xSemaphoreTakeRecursive(bacnet_datalink_mutex, pdMS_TO_TICKS(2500)) != pdTRUE) {
        return false;
    }

    if (name != NULL) {
        datalink_set((char *)name);
    }
    return true;
}

static void bacnet_log_task_create_request(const char *task_name, unsigned stack_bytes, int core_id)
{
    ESP_LOGI(TAG, "Task create request: name=%s stack=%u core=%d", task_name, stack_bytes, core_id);
}

static void bacnet_log_task_create_result(const char *task_name, TaskHandle_t task_handle, unsigned stack_bytes, int core_id)
{
    ESP_LOGI(TAG, "Task created: name=%s handle=%p stack=%u core=%d", task_name, (void *)task_handle, stack_bytes, core_id);
}

void bacnet_datalink_unlock(void)
{
    if (xPortInIsrContext() || !bacnet_datalink_mutex) {
        return;
    }

    TaskHandle_t holder = xSemaphoreGetMutexHolder(bacnet_datalink_mutex);
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (holder != self) {
        return;
    }

    if (datalink_default) {
        datalink_set(datalink_default);
    }

    if (xSemaphoreGiveRecursive(bacnet_datalink_mutex) != pdTRUE) {
        return;
    }
}

static bool bacnet_mstp_init(void)
{
    MSTP_RS485_Init();

    memset(&mstp_port, 0, sizeof(mstp_port));
    memset(&mstp_user, 0, sizeof(mstp_user));

    mstp_user.RS485_Driver = &mstp_rs485_driver;
    mstp_port.UserData = &mstp_user;
    mstp_port.InputBuffer = mstp_rx_buffer;
    mstp_port.InputBufferSize = sizeof(mstp_rx_buffer);
    mstp_port.OutputBuffer = mstp_tx_buffer;
    mstp_port.OutputBufferSize = sizeof(mstp_tx_buffer);

    dlmstp_set_interface((const char *)&mstp_port);
    dlmstp_set_mac_address(USER_MSTP_MAC_ADDRESS);
    dlmstp_set_max_info_frames(USER_MSTP_MAX_INFO_FRAMES);
    dlmstp_set_max_master(USER_MSTP_MAX_MASTER);
    dlmstp_set_baud_rate(USER_MSTP_BAUD_RATE);
    dlmstp_slave_mode_enabled_set(false);

    bool ok = dlmstp_init((char *)&mstp_port);
#if USER_ENABLE_RGB_STATUS
    if (ok) {
        rgb_status_set_mstp_ready();
    }
#endif
    return ok;
}

/* BACnet receive task - processes incoming BACnet messages */
static void bacnet_receive_task(void *pvParameters)
{
    (void)pvParameters;
    BACNET_ADDRESS src = {0};
    static uint8_t rx_buffer[600];  /* Smaller buffer in DRAM */
    uint16_t pdu_len = 0;

    ESP_LOGI(TAG, "BACnet receive task started");

    while (1) {
        if (!bacnet_ip_initialized) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Poll for incoming BACnet messages */
        memset(&src, 0, sizeof(src));
        pdu_len = bip_receive(&src, rx_buffer, sizeof(rx_buffer), 100);
        if (pdu_len > 0) {
            /* Save original source from UDP socket before NPDU decode modifies it */
            BACNET_ADDRESS orig_src = src;
            BACNET_ADDRESS dest = {0};
            BACNET_NPDU_DATA npdu_data = {0};
            int apdu_offset = bacnet_npdu_decode(
                rx_buffer, pdu_len, &dest, &src, &npdu_data);
            /* If NPDU didn't have source routing info, restore from UDP socket */
            if (src.len == 0) {
                src = orig_src;
            }
            if (apdu_offset > 0 && apdu_offset < (int)pdu_len) {
                ESP_LOGD(TAG, "Received BACnet APDU (bip), len=%u", (unsigned)(pdu_len - apdu_offset));
                if (bacnet_datalink_lock(datalink_bip)) {
                    apdu_handler(&src, &rx_buffer[apdu_offset], pdu_len - apdu_offset);
                    bacnet_datalink_unlock();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void bacnet_start_receive_task(void)
{
    if (bacnet_rx_task_handle != NULL) {
        return;
    }

    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "Heap before bacnet_rx create: free=%u largest=%u current_task_stack_hwm=%u", (unsigned)free_bytes, (unsigned)largest, (unsigned)hwm);
    bacnet_rx_task_handle = xTaskCreateStaticPinnedToCore(
        bacnet_receive_task,
        "bacnet_rx",
        (16384 / sizeof(StackType_t)),
        NULL,
        5,
        bacnet_rx_stack,
        &bacnet_rx_task_buffer,
        BACNET_TASK_CORE);
    if (bacnet_rx_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create bacnet_rx static task");
    } else {
        ESP_LOGI(TAG, "bacnet_rx static task started");
    }
}

/* BACnet MS/TP receive task - processes incoming MS/TP frames */
static void __attribute__((unused)) bacnet_mstp_receive_task(void *pvParameters)
{
    (void)pvParameters;
    BACNET_ADDRESS src = {0};
    static uint8_t rx_buffer[600];
    uint16_t pdu_len = 0;

    ESP_LOGI(TAG, "BACnet MS/TP receive task started");

    while (1) {
        memset(&src, 0, sizeof(src));
        pdu_len = dlmstp_receive(&src, rx_buffer, sizeof(rx_buffer), 0);
        if (pdu_len > 0) {
            mstp_pdu_count++;
            /* MS/TP activity no longer flashes RGB; persistent MS/TP ready state is set when interface initializes */
            BACNET_ADDRESS dest = {0};
            BACNET_NPDU_DATA npdu_data = {0};
            int apdu_offset = bacnet_npdu_decode(
                rx_buffer, pdu_len, &dest, &src, &npdu_data);
            if (apdu_offset > 0 && apdu_offset < (int)pdu_len) {
                mstp_apdu_count++;
                if ((apdu_offset + 4) <= (int)pdu_len) {
                    uint8_t pdu_type = rx_buffer[apdu_offset] & 0xF0;
                    uint8_t service_choice = rx_buffer[apdu_offset + 3];
                    if (pdu_type == PDU_TYPE_CONFIRMED_SERVICE_REQUEST &&
                        service_choice == SERVICE_CONFIRMED_READ_PROPERTY) {
                        mstp_rp_total++;
                        mstp_rp_last_value = Analog_Value_Present_Value(1);
                    } else if (pdu_type == PDU_TYPE_CONFIRMED_SERVICE_REQUEST &&
                        service_choice == SERVICE_CONFIRMED_WRITE_PROPERTY) {
                        mstp_wp_total++;
                    }
                }
                ESP_LOGD(TAG, "Received BACnet APDU (mstp), len=%u", (unsigned)(pdu_len - apdu_offset));
                if (bacnet_datalink_lock(datalink_mstp)) {
                    apdu_handler(&src, &rx_buffer[apdu_offset], pdu_len - apdu_offset);
                    bacnet_datalink_unlock();
                }
            } else {
                ESP_LOGW(TAG, "MS/TP RX frame decode failed: len=%u apdu_offset=%d src.len=%u src.mac=%u",
                    (unsigned)pdu_len, apdu_offset, (unsigned)src.len,
                    (unsigned)(src.len ? src.mac[0] : 0));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* Removed: duplicate old app_main() - preserved the new staged app_main() at file top. */

/* COV task - handles COV timer and notifications */
static void __attribute__((unused)) bacnet_cov_task(void *pvParameters)
{
    (void)pvParameters;
    while (1) {
        char *active_datalink = datalink_default;
        if (!active_datalink) {
            if (bacnet_ip_initialized && (USER_ENABLE_BACNET_IP_ETHERNET || USER_ENABLE_BACNET_IP_WIFI)) {
                active_datalink = datalink_bip;
            } else if (USER_ENABLE_BACNET_MSTP) {
                active_datalink = datalink_mstp;
            }
        }

        if (active_datalink) {
            if (bacnet_datalink_lock(active_datalink)) {
                handler_cov_timer_seconds(1);
                handler_cov_task();
                bacnet_datalink_unlock();
            }
        } else {
            handler_cov_timer_seconds(1);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static bool bacnet_init_objects_once(void)
{
    if (bacnet_objects_ready) {
        return true;
    }

    ESP_LOGI(TAG, "[FSM] OBJECT_INIT starting");
    ESP_LOGI(TAG, "[FSM] Heap before BACnet objects: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (USER_ENABLE_BACNET_IP_WIFI && !USER_ENABLE_ETHERNET) {
        datalink_default = datalink_bip;
    }
#if !SKIP_PERIPHERALS
    else if (USER_ENABLE_BACNET_MSTP) {
        datalink_default = datalink_mstp;
    }
#endif
    if (datalink_default) {
        datalink_set(datalink_default);
    }

    Device_Init(NULL);
    Device_Set_Object_Instance_Number(USER_BACNET_DEVICE_INSTANCE);
    Device_Set_Vendor_Identifier(260);
    Device_Object_Name_ANSI_Init(USER_BACNET_DEVICE_NAME);

    ESP_LOGI(TAG, "Registering BACnet service handlers");
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_i_am_add);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE, handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY, handler_write_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV_PROPERTY, handler_cov_subscribe_property);

    handler_cov_init();
    bacnet_create_analog_values();
    bacnet_create_binary_values();
    bacnet_create_analog_inputs();
    bacnet_create_binary_inputs();
    bacnet_create_binary_outputs_with_gpio_sync();

    {
        bool object_tables_ready = (Analog_Value_Count() == USER_AV_COUNT) &&
            (Binary_Value_Count() == USER_BV_COUNT) &&
            (Analog_Input_Count() == USER_AI_COUNT) &&
            (Binary_Input_Count() == USER_BI_COUNT) &&
            (Binary_Output_Count() == USER_BO_COUNT);

        if (!object_tables_ready) {
            ESP_LOGE(TAG,
                     "BACnet object tables incomplete after init (AV=%u/%u BV=%u/%u AI=%u/%u BI=%u/%u BO=%u/%u)",
                     (unsigned)Analog_Value_Count(), (unsigned)USER_AV_COUNT,
                     (unsigned)Binary_Value_Count(), (unsigned)USER_BV_COUNT,
                     (unsigned)Analog_Input_Count(), (unsigned)USER_AI_COUNT,
                     (unsigned)Binary_Input_Count(), (unsigned)USER_BI_COUNT,
                     (unsigned)Binary_Output_Count(), (unsigned)USER_BO_COUNT);
            return false;
        }
    }

    ESP_LOGI(TAG, "Initializing bacnet_io link and startup task");
    bacnet_io_link_init();
    if (bacnet_io_task_handle == NULL) {
        bacnet_log_task_create_request("bacnet_io", 4096, BACNET_TASK_CORE);
        bacnet_io_task_handle = xTaskCreateStaticPinnedToCore(
            bacnet_io_link_task,
            "bacnet_io",
            (4096 / sizeof(StackType_t)),
            NULL,
            tskIDLE_PRIORITY + 2,
            bacnet_io_stack,
            &bacnet_io_task_buffer,
            BACNET_TASK_CORE);
        if (bacnet_io_task_handle == NULL) {
            ESP_LOGE(TAG, "Failed to create bacnet_io_link static task");
            return false;
        }
        bacnet_log_task_create_result("bacnet_io", bacnet_io_task_handle, 4096, BACNET_TASK_CORE);
    }

    bacnet_objects_ready = true;
    return true;
}

static bool bacnet_init_mstp_once(void)
{
    if (!USER_ENABLE_BACNET_MSTP) {
        return false;
    }
    if (bacnet_mstp_initialized_once) {
        return true;
    }

    if (!bacnet_mstp_init()) {
        ESP_LOGW(TAG, "BACnet MS/TP init failed; MSTP task not started");
        return false;
    }

    if (bacnet_mstp_task_handle != NULL) {
        bacnet_mstp_initialized_once = true;
        return true;
    }

    bacnet_log_task_create_request("bacnet_mstp", 4096, BACNET_TASK_CORE);
    BaseType_t created = xTaskCreatePinnedToCore(
        bacnet_mstp_receive_task, "bacnet_mstp", 4096, NULL, 5,
        &bacnet_mstp_task_handle, BACNET_TASK_CORE);
    if (created != pdPASS || bacnet_mstp_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create bacnet_mstp task (created=%d handle=%p)",
                 (int)created, (void *)bacnet_mstp_task_handle);
        bacnet_mstp_task_handle = NULL;
        return false;
    }

    bacnet_log_task_create_result("bacnet_mstp", bacnet_mstp_task_handle, 4096, BACNET_TASK_CORE);
    bacnet_mstp_initialized_once = true;
    datalink_default = datalink_mstp;
    return true;
}

/* Called by bacnet_gateway when transport becomes active or interface changes.
 * I-Am is sent automatically by the gateway; this hook handles side effects. */
static void bacnet_on_transport_active(void)
{
#if USER_ENABLE_RGB_STATUS
    rgb_status_set_bacnet_ready();
#endif
}

static void bacnet_register_with_bbmd(void)
{
    BACNET_IP_ADDRESS bbmd_addr = { { USER_BBMD_IP_OCTET_1, USER_BBMD_IP_OCTET_2,
                                     USER_BBMD_IP_OCTET_3, USER_BBMD_IP_OCTET_4 },
                                    USER_BBMD_PORT };
    int result = bvlc_register_with_bbmd(&bbmd_addr, USER_BBMD_TTL_SECONDS);
    if (result >= 0) {
        ESP_LOGI(TAG, "BBMD register OK: result=%d ttl=%u", result,
                 (unsigned)USER_BBMD_TTL_SECONDS);
    } else {
        ESP_LOGW(TAG, "BBMD register failed: result=%d ttl=%u", result,
                 (unsigned)USER_BBMD_TTL_SECONDS);
    }
    /* BBMD registration no longer changes RGB status */
}

static void bacnet_bip_transport_ready(void)
{
    if (bacnet_ip_initialized) {
        return;
    }

    bacnet_ip_initialized = true;
    datalink_default = datalink_bip;
    bacnet_register_with_bbmd();
    bacnet_start_receive_task();
}

void bacnet_network_notify(uint32_t event_bits)
{
    if (bacnet_network_event_group) {
        xEventGroupSetBits(bacnet_network_event_group, event_bits);
    }
}

static void bacnet_dump_net_state(const char *context)
{
    esp_netif_t *eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t eth_ip = {0};
    esp_netif_ip_info_t wifi_ip = {0};
    wifi_mode_t wifi_mode = WIFI_MODE_NULL;
    bool wifi_initialized = app_wifi_is_initialized();
    bool wifi_started = app_wifi_is_started();
    bool wifi_connected = app_wifi_is_connected();
    bool eth_link = w5500_eth_link_up();
    bool eth_ip_valid = false;
    bool wifi_ip_valid = false;
    bool eth_usable = false;
    bool wifi_usable = false;
    const char *selected = "NONE";
    const char *active = bacnet_fsm_get_active_transport_name();

    (void)esp_wifi_get_mode(&wifi_mode);
    if (eth_netif) {
        (void)esp_netif_get_ip_info(eth_netif, &eth_ip);
    }
    if (wifi_netif) {
        (void)esp_netif_get_ip_info(wifi_netif, &wifi_ip);
    }
    eth_ip_valid = eth_ip.ip.addr != 0;
    wifi_ip_valid = wifi_ip.ip.addr != 0;
    eth_usable = eth_link && eth_ip_valid;
    wifi_usable = wifi_connected && wifi_ip_valid;
    selected = bacnet_selected_transport_name(eth_usable, wifi_usable);

    ESP_LOGI(TAG,
             "NET_DIAG[%s] heap_free=%u heap_largest=%u eth_link=%d eth_ip=%u.%u.%u.%u eth_usable=%d wifi_mode=%d wifi_connected=%d wifi_ip=%u.%u.%u.%u wifi_usable=%d",
             context,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             eth_link ? 1 : 0,
             (unsigned)((ntohl(eth_ip.ip.addr) >> 24) & 0xFF),
             (unsigned)((ntohl(eth_ip.ip.addr) >> 16) & 0xFF),
             (unsigned)((ntohl(eth_ip.ip.addr) >> 8) & 0xFF),
             (unsigned)(ntohl(eth_ip.ip.addr) & 0xFF),
             eth_usable ? 1 : 0,
             (int)wifi_mode,
             wifi_connected ? 1 : 0,
             (unsigned)((ntohl(wifi_ip.ip.addr) >> 24) & 0xFF),
             (unsigned)((ntohl(wifi_ip.ip.addr) >> 16) & 0xFF),
             (unsigned)((ntohl(wifi_ip.ip.addr) >> 8) & 0xFF),
             (unsigned)(ntohl(wifi_ip.ip.addr) & 0xFF),
             wifi_usable ? 1 : 0);

    ESP_LOGI(TAG,
             "FAILOVER: ETH=%s ETH_IP=%u.%u.%u.%u WIFI=%s WIFI_IP=%u.%u.%u.%u eth_usable=%d wifi_usable=%d selected=%s active=%s",
             eth_link ? "up" : "down",
             (unsigned)((ntohl(eth_ip.ip.addr) >> 24) & 0xFF),
             (unsigned)((ntohl(eth_ip.ip.addr) >> 16) & 0xFF),
             (unsigned)((ntohl(eth_ip.ip.addr) >> 8) & 0xFF),
             (unsigned)(ntohl(eth_ip.ip.addr) & 0xFF),
             wifi_connected ? "connected" : "disconnected",
             (unsigned)((ntohl(wifi_ip.ip.addr) >> 24) & 0xFF),
             (unsigned)((ntohl(wifi_ip.ip.addr) >> 16) & 0xFF),
             (unsigned)((ntohl(wifi_ip.ip.addr) >> 8) & 0xFF),
             (unsigned)(ntohl(wifi_ip.ip.addr) & 0xFF),
             eth_usable ? 1 : 0,
             wifi_usable ? 1 : 0,
             selected,
             active);

    ESP_LOGI(TAG,
             "FAILOVER_STATE[%s] wifi_initialized=%d wifi_started=%d wifi_connected=%d wifi_ip_valid=%d eth_link=%d eth_ip_valid=%d active_ifkey=%s bip_initialized=%d",
             context,
             wifi_initialized ? 1 : 0,
             wifi_started ? 1 : 0,
             wifi_connected ? 1 : 0,
             wifi_ip_valid ? 1 : 0,
             eth_link ? 1 : 0,
             eth_ip_valid ? 1 : 0,
             bacnet_active_ifkey ? bacnet_active_ifkey : "NONE",
             bacnet_bip_initialized ? 1 : 0);
}

static const char *bacnet_selected_transport_name(bool eth_usable, bool wifi_usable)
{
    if (USER_ENABLE_BACNET_IP_ETHERNET && eth_usable) {
        return "Ethernet";
    }

    if (USER_ENABLE_BACNET_IP_WIFI && wifi_usable) {
        return "WiFi";
    }

    if (USER_ENABLE_BACNET_MSTP && bacnet_mstp_initialized_once) {
        return "MS/TP";
    }

    return "NONE";
}

static esp_err_t bacnet_start_bip_on_interface(const char *ifkey, const char *label)
{
    if (!bacnet_update_bip_addresses(ifkey, label)) {
        return ESP_FAIL;
    }

    bool first_start = !bacnet_bip_initialized;
    bool migrating = bacnet_bip_initialized &&
                     bacnet_active_ifkey &&
                     (strcmp(bacnet_active_ifkey, ifkey) != 0);

    if (first_start || migrating) {
        datalink_set(datalink_bip);
        if (migrating) {
            bip_cleanup();
        }
        if (!datalink_init((char *)ifkey)) {
            ESP_LOGE(TAG, "Failed to %s BACnet datalink on %s",
                     migrating ? "migrate" : "initialize", label);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "BACnet transport switched: %s", label);
        bacnet_bip_initialized = true;
        if (first_start) {
            bacnet_bip_transport_ready();
        }
    }

    bacnet_active_ifkey = ifkey;
    datalink_default = datalink_bip;
    return ESP_OK;
}

static bool bacnet_update_bip_addresses(const char *ifkey, const char *label)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (netif == NULL) {
        ESP_LOGW(TAG, "BACnet/IP cannot use %s: netif %s not found", label, ifkey);
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        ESP_LOGW(TAG, "BACnet/IP cannot use %s: no valid IP", label);
        return false;
    }

    uint32_t ip_addr_val = ntohl(ip_info.ip.addr);
    uint32_t netmask_val = ntohl(ip_info.netmask.addr);
    uint32_t broadcast_val = (ip_addr_val & netmask_val) | (~netmask_val);

    BACNET_IP_ADDRESS my_addr = {{0, 0, 0, 0}, 0xBAC0};
    BACNET_IP_ADDRESS bcast_addr = {{255, 255, 255, 255}, 0xBAC0};

    my_addr.address[0] = (uint8_t)((ip_addr_val >> 24) & 0xFF);
    my_addr.address[1] = (uint8_t)((ip_addr_val >> 16) & 0xFF);
    my_addr.address[2] = (uint8_t)((ip_addr_val >> 8) & 0xFF);
    my_addr.address[3] = (uint8_t)(ip_addr_val & 0xFF);

    bcast_addr.address[0] = (uint8_t)((broadcast_val >> 24) & 0xFF);
    bcast_addr.address[1] = (uint8_t)((broadcast_val >> 16) & 0xFF);
    bcast_addr.address[2] = (uint8_t)((broadcast_val >> 8) & 0xFF);
    bcast_addr.address[3] = (uint8_t)(broadcast_val & 0xFF);

    bip_set_addr(&my_addr);
    bip_set_broadcast_addr(&bcast_addr);

    ESP_LOGD(TAG, "BACnet/IP %s selected: ip=%u.%u.%u.%u bcast=%u.%u.%u.%u",
             label,
             (unsigned)my_addr.address[0],
             (unsigned)my_addr.address[1],
             (unsigned)my_addr.address[2],
             (unsigned)my_addr.address[3],
             (unsigned)bcast_addr.address[0],
             (unsigned)bcast_addr.address[1],
             (unsigned)bcast_addr.address[2],
             (unsigned)bcast_addr.address[3]);
    return true;
}

static bool bacnet_is_eth_usable(void)
{
    if (!USER_ENABLE_BACNET_IP_ETHERNET || !USER_ENABLE_ETHERNET) {
        return false;
    }

    if (!w5500_eth_link_up()) {
        return false;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!netif) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }

    return ip_info.ip.addr != 0;
}

static bool bacnet_is_wifi_usable(void)
{
    if (!USER_ENABLE_BACNET_IP_WIFI) {
        return false;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return false;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }

    return ip_info.ip.addr != 0;
}

static void bacnet_eth_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == ETH_EVENT) {
        if (event_id == ETHERNET_EVENT_DISCONNECTED || event_id == ETHERNET_EVENT_STOP) {
            ESP_LOGI(TAG, "FAILOVER_PATH: ETH_EVENT_%s", event_id == ETHERNET_EVENT_DISCONNECTED ? "DISCONNECTED" : "STOP");
            bacnet_dump_net_state("eth_disconnect_before_notify");
            bacnet_network_notify(BACNET_NETWORK_EVT_ETH_LOST_IP);
            bacnet_dump_net_state("eth_disconnect_after_notify");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ESP_LOGI(TAG, "FAILOVER_PATH: IP_EVENT_ETH_GOT_IP");
        bacnet_dump_net_state("eth_got_ip");
        bacnet_network_notify(BACNET_NETWORK_EVT_ETH_GOT_IP);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGI(TAG, "FAILOVER_PATH: IP_EVENT_ETH_LOST_IP");
        bacnet_dump_net_state("eth_lost_ip");
    }
}

static void bacnet_network_task(void *pvParameters)
{
    (void)pvParameters;
    bool last_eth_usable = false;
    bool last_wifi_usable = false;

    ESP_LOGI(TAG, "BACnet network task running: handle=%p core=%d", (void *)xTaskGetCurrentTaskHandle(), xPortGetCoreID());
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            bacnet_network_event_group,
            BACNET_NETWORK_EVT_ETH_GOT_IP |
            BACNET_NETWORK_EVT_ETH_LOST_IP |
            BACNET_NETWORK_EVT_WIFI_GOT_IP |
            BACNET_NETWORK_EVT_WIFI_LOST_IP,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(500));

        if (bits != 0) {
            ESP_LOGI(TAG, "FAILOVER_PATH: network_task bits=0x%lx", (unsigned long)bits);
        }

        bool eth_usable = bacnet_is_eth_usable();
        if (eth_usable != last_eth_usable) {
            last_eth_usable = eth_usable;
            if (eth_usable) {
                ESP_LOGI(TAG, "Ethernet connected");
                bacnet_fsm_handle_event(BACNET_FSM_EVENT_ETH_UP);
            } else {
                ESP_LOGI(TAG, "Ethernet disconnected");
                bacnet_fsm_handle_event(BACNET_FSM_EVENT_ETH_DOWN);
            }
            bacnet_dump_net_state("eth_usable_changed");
        }

        bool wifi_usable = bacnet_is_wifi_usable();
        if (wifi_usable != last_wifi_usable) {
            last_wifi_usable = wifi_usable;
            if (wifi_usable) {
                ESP_LOGI(TAG, "WiFi connected");
                bacnet_fsm_handle_event(BACNET_FSM_EVENT_WIFI_UP);
            } else {
                ESP_LOGI(TAG, "WiFi disconnected");
                bacnet_fsm_handle_event(BACNET_FSM_EVENT_WIFI_DOWN);
            }
            bacnet_dump_net_state("wifi_usable_changed");
        }
    }
}
