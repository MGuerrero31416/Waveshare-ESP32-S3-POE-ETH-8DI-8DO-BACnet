#include "rgb_status.h"

#if USER_ENABLE_RGB_STATUS

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "led_strip.h"

static const char *TAG = "rgb_status";

/* GPIO for on-board NeoPixel */
#define RGB_GPIO_NUM GPIO_NUM_38

/* Task / queue configuration */
#define RGB_TASK_STACK_SZ 4096
#define RGB_TASK_PRIO (tskIDLE_PRIORITY + 2)
#define RGB_QUEUE_LEN 8

typedef enum {
    RGB_CMD_SET_BOOTING,
    RGB_CMD_SET_WAITING_FOR_IP,
    RGB_CMD_SET_BACNET_READY,
    RGB_CMD_SET_MSTP_READY,
    RGB_CMD_SET_ERROR,
} rgb_cmd_t;

typedef struct {
    rgb_cmd_t cmd;
} rgb_msg_t;

static QueueHandle_t rgb_queue = NULL;
static TaskHandle_t rgb_task_handle = NULL;

static led_strip_t *strip = NULL;

/* Base state remembered so persistent states can be tracked */
typedef enum { BASE_BOOT, BASE_WAIT_IP, BASE_BACNET_READY, BASE_MSTP_READY, BASE_ERROR } base_state_t;
static base_state_t current_base = BASE_BOOT;


/* Helper: set pixel color (R,G,B) and refresh. Non-blocking relative to rest of system.
 * Uses led_strip API. */
/* WS2812 RMT-based pixel send: GRB ordering expected by strip */
/* Single helper that performs the channel swap required by the Waveshare
 * panel: the onboard WS2812 expects GRB order on-wire (factory call:
 * neopixelWrite(GPIO_PIN_RGB, green, red, blue)). We accept (r,g,b)
 * from the rest of the module and perform the swap here so all callers
 * remain unchanged.
 */
static void set_rgb_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    if (!strip) return;
    /* Swap R and G to produce GRB on-wire */
    uint8_t send_r = g;
    uint8_t send_g = r;
    uint8_t send_b = b;
#if USER_RGB_DEBUG_LOGS
    ESP_LOGI("RGB_TRACE", "set_rgb_pixel() called: r=%u g=%u b=%u -> send_r=%u send_g=%u send_b=%u",
             (unsigned)r, (unsigned)g, (unsigned)b,
             (unsigned)send_r, (unsigned)send_g, (unsigned)send_b);
#endif
    led_strip_set_pixel(strip, 0, send_r, send_g, send_b);
    led_strip_refresh(strip, 100);
}

/* Map base state to color/pattern (solid handled by task loop) */
static void apply_base_state(base_state_t s)
{
    /* Apply base immediately and log a single informational message when the base state changes. */
    base_state_t prev = current_base;
    if (prev != s) {
        /* Log one informational line per state change */
        switch (s) {
            case BASE_BOOT:
                ESP_LOGI(TAG, "RGB: Booting");
                break;
            case BASE_WAIT_IP:
                ESP_LOGI(TAG, "RGB: Waiting for IP");
                break;
            case BASE_BACNET_READY:
                ESP_LOGI(TAG, "RGB: BACnet Ready");
                break;
            case BASE_MSTP_READY:
                ESP_LOGI(TAG, "RGB: MSTP Ready");
                break;
            case BASE_ERROR:
                ESP_LOGI(TAG, "RGB: Error");
                break;
        }
    }
    current_base = s;
    switch (s) {
        case BASE_BOOT:
            set_rgb_pixel(0, 0, 255); /* Blue solid */
            break;
        case BASE_WAIT_IP:
            /* Yellow slow blink handled by periodic task loop: set on now */
            set_rgb_pixel(255, 255, 0);
            break;
        case BASE_BACNET_READY:
            set_rgb_pixel(0, 255, 0); /* Green solid */
            break;
        case BASE_MSTP_READY:
            set_rgb_pixel(0, 255, 255); /* Cyan solid for MS/TP ready */
            break;
        case BASE_ERROR:
            set_rgb_pixel(255, 0, 0); /* Red fast blink handled by loop */
            break;
    }
}

static void rgb_task(void *arg)
{
    (void)arg;
    rgb_msg_t msg;
    const TickType_t wait_ticks = pdMS_TO_TICKS(50);
    int blink_phase = 0; /* used for blinking */

    while (1) {
        /* Process queued commands */
        while (xQueueReceive(rgb_queue, &msg, 0) == pdTRUE) {
#if USER_RGB_DEBUG_LOGS
            ESP_LOGI("RGB_TRACE", "rgb_task: dequeued cmd=%d", (int)msg.cmd);
#endif
            switch (msg.cmd) {
                case RGB_CMD_SET_BOOTING:
                    apply_base_state(BASE_BOOT);
                    break;
                case RGB_CMD_SET_WAITING_FOR_IP:
                    apply_base_state(BASE_WAIT_IP);
                    break;
                case RGB_CMD_SET_BACNET_READY:
                    apply_base_state(BASE_BACNET_READY);
                    break;
                case RGB_CMD_SET_MSTP_READY:
                    apply_base_state(BASE_MSTP_READY);
                    break;
                case RGB_CMD_SET_ERROR:
                    apply_base_state(BASE_ERROR);
                    break;
            }
        }

        /* Handle blinking patterns for WAIT_IP (1s on/off) and ERROR fast blink (250ms)
         * We implement by toggling visible vs off state at appropriate intervals.
         */
        if (current_base == BASE_WAIT_IP) {
            /* slow blink 1s on/off */
            blink_phase = (blink_phase + 1) & 0xFFFF;
            if ((blink_phase % 20) == 0) { /* ~1000ms if tick 50ms */
                /* toggle: when even show color, when odd clear */
                set_rgb_pixel(255, 255, 0);
            } else if ((blink_phase % 20) == 10) {
                set_rgb_pixel(0,0,0);
            }
        } else if (current_base == BASE_ERROR) {
            /* fast blink 250ms on/off */
            blink_phase = (blink_phase + 1) & 0xFFFF;
            if ((blink_phase % 5) == 0) { /* ~250ms */
                set_rgb_pixel(255, 0, 0);
            } else if ((blink_phase % 5) == 2) {
                set_rgb_pixel(0,0,0);
            }
        }

        vTaskDelay(wait_ticks);
    }
}

/* Public API: enqueue messages (non-blocking) */
static void enqueue_cmd(rgb_cmd_t cmd)
{
    if (!rgb_queue) return;
    rgb_msg_t m = { .cmd = cmd };
#if USER_RGB_DEBUG_LOGS
    ESP_LOGI("RGB_TRACE", "enqueue_cmd() attempting to enqueue cmd=%d", (int)cmd);
#endif
    BaseType_t res = xQueueSend(rgb_queue, &m, 0); /* do not block */
    if (res == pdTRUE) {
#if USER_RGB_DEBUG_LOGS
        UBaseType_t depth = uxQueueMessagesWaiting(rgb_queue);
        ESP_LOGI("RGB_TRACE", "enqueue_cmd() success cmd=%d queue_depth=%u", (int)cmd, (unsigned)depth);
#endif
    } else {
#if USER_RGB_DEBUG_LOGS
        ESP_LOGW("RGB_TRACE", "enqueue_cmd() FAILED cmd=%d (queue full)", (int)cmd);
#endif
    }
}

void rgb_status_init(void)
{
#if USER_RGB_DEBUG_LOGS
    ESP_LOGI(TAG, "RGB Status enabled");
    ESP_LOGI(TAG, "RGB GPIO=%d", 38);

    ESP_LOGI(TAG, "Initializing RGB status on GPIO %d", GPIO_NUM_38);
#endif

    /* Configure led_strip device */
    led_strip_config_t cfg = {
        .max_leds = 1,
        .gpio_num = RGB_GPIO_NUM,
        .led_model = LED_STRIP_WS2812,
    };

    strip = led_strip_new_rmt_device(&cfg);
    if (!strip) {
        ESP_LOGE(TAG, "Failed to create led_strip instance");
    } else {
        led_strip_init(strip);
        led_strip_clear(strip);
        led_strip_refresh(strip, 100);
    }

    rgb_queue = xQueueCreate(RGB_QUEUE_LEN, sizeof(rgb_msg_t));
    if (!rgb_queue) {
        ESP_LOGE(TAG, "Failed to create RGB queue");
    }

    if (rgb_task_handle == NULL) {
        BaseType_t created = xTaskCreatePinnedToCore(
            rgb_task, "rgb_status", RGB_TASK_STACK_SZ, NULL, RGB_TASK_PRIO,
            &rgb_task_handle, 0);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create rgb_status task");
            rgb_task_handle = NULL;
        }
    } else {
        ESP_LOGW(TAG, "rgb_status task already running; skipping create");
    }

    /* Default base state: booting */
    enqueue_cmd(RGB_CMD_SET_BOOTING);
}

void rgb_status_set_booting(void) { enqueue_cmd(RGB_CMD_SET_BOOTING); }
void rgb_status_set_waiting_for_ip(void) { enqueue_cmd(RGB_CMD_SET_WAITING_FOR_IP); }
void rgb_status_set_bacnet_ready(void) { enqueue_cmd(RGB_CMD_SET_BACNET_READY); }
void rgb_status_set_mstp_ready(void) { enqueue_cmd(RGB_CMD_SET_MSTP_READY); }
void rgb_status_set_error(void) { enqueue_cmd(RGB_CMD_SET_ERROR); }
void rgb_status_bbmd_registered(void) { /* intentionally no-op: BBMD no longer triggers LED transient */ }

#else
/* Should not reach here because header provides stubs when disabled */
#endif
