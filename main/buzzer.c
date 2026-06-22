/* LEDC-based non-blocking buzzer implementation
 * Uses LEDC (PWM) at 1 kHz, 8-bit resolution. Duty 200 when active.
 * Beep requests are queued to a FreeRTOS task so calls are non-blocking.
 */

#include "buzzer.h"
#include "User_Settings.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "buzzer";
#define BUZZER_GPIO_NUM 46
#define BUZZER_FREQ_HZ 1000
#define BUZZER_RES_BITS LEDC_TIMER_8_BIT
#define BUZZER_DUTY_ACTIVE 200
#define BUZZER_QUEUE_LEN 8
#define BUZZER_MIN_MS 10
#define BUZZER_MAX_MS 1000

static QueueHandle_t buzzer_queue = NULL;
static TaskHandle_t buzzer_task_handle = NULL;

static void buzzer_task(void *pvParameters)
{
    (void)pvParameters;
    uint32_t dur_ms;
    for (;;) {
        if (xQueueReceive(buzzer_queue, &dur_ms, portMAX_DELAY) == pdTRUE) {
            if (!USER_BUZZER_ENABLE) continue;
            if (dur_ms < BUZZER_MIN_MS) dur_ms = BUZZER_MIN_MS;
            if (dur_ms > BUZZER_MAX_MS) dur_ms = BUZZER_MAX_MS;

            /* Set duty to active and update */
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, BUZZER_DUTY_ACTIVE);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

            vTaskDelay(pdMS_TO_TICKS(dur_ms));

            /* Turn off */
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        }
    }
}

void buzzer_init(void)
{
    /* Configure LEDC timer */
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = BUZZER_RES_BITS,
        .freq_hz = BUZZER_FREQ_HZ,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    /* Configure LEDC channel */
    ledc_channel_config_t ledc_ch = {
        .channel = LEDC_CHANNEL_0,
        .duty = 0,
        .gpio_num = BUZZER_GPIO_NUM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint = 0,
        .timer_sel = LEDC_TIMER_0
    };
    ledc_channel_config(&ledc_ch);

    /* Create queue and task */
    if (buzzer_queue == NULL) {
        buzzer_queue = xQueueCreate(BUZZER_QUEUE_LEN, sizeof(uint32_t));
    }
    if (buzzer_queue && buzzer_task_handle == NULL) {
        BaseType_t created = xTaskCreate(
            buzzer_task, "buzzer", 2048, NULL, tskIDLE_PRIORITY + 2,
            &buzzer_task_handle);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create buzzer task");
            buzzer_task_handle = NULL;
        }
    } else if (buzzer_task_handle != NULL) {
        ESP_LOGW(TAG, "Buzzer task already running; skipping create");
    }

    ESP_LOGI(TAG, "Buzzer initialized on GPIO%d", BUZZER_GPIO_NUM);
    ESP_LOGI(TAG, "Buzzer enabled=%d beep_ms=%d", USER_BUZZER_ENABLE, USER_BUZZER_BEEP_MS);
}

void buzzer_beep(uint32_t duration_ms)
{
    if (!buzzer_queue) return;
    /* Non-blocking enqueue; drop if queue full */
    xQueueSend(buzzer_queue, &duration_ms, 0);
}

bool buzzer_is_enabled(void)
{
    return USER_BUZZER_ENABLE != 0;
}

uint32_t buzzer_get_default_ms(void)
{
    return (uint32_t)USER_BUZZER_BEEP_MS;
}
