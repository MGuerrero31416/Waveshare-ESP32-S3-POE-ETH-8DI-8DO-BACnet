#include "led_strip.h"
#include <stdlib.h>
#include <string.h>
#include "driver/rmt.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "led_strip_stub";

struct led_strip_s {
    uint16_t max_leds;
    gpio_num_t gpio_num;
    led_strip_model_t model;
    uint8_t *pixels; /* RGB bytes for each led */
    rmt_channel_t channel;
    bool inited;
};

led_strip_t* led_strip_new_rmt_device(const led_strip_config_t *config)
{
    if (!config) return NULL;
    led_strip_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->max_leds = config->max_leds;
    s->gpio_num = config->gpio_num;
    s->model = config->led_model;
    s->pixels = calloc(3 * s->max_leds, 1);
    s->channel = RMT_CHANNEL_0;
    s->inited = false;
    return s;
}

void led_strip_init(led_strip_t *strip)
{
    if (!strip) return;
    rmt_config_t rmt_tx = RMT_DEFAULT_CONFIG_TX(strip->gpio_num, strip->channel);
    rmt_tx.clk_div = 2; /* fine-grain timing */
    esp_err_t err = rmt_config(&rmt_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_config failed: %s", esp_err_to_name(err));
        return;
    }
    err = rmt_driver_install(strip->channel, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_driver_install failed: %s", esp_err_to_name(err));
        return;
    }
    strip->inited = true;
}

void led_strip_clear(led_strip_t *strip)
{
    if (!strip || !strip->pixels) return;
    memset(strip->pixels, 0, 3 * strip->max_leds);
}

void led_strip_set_pixel(led_strip_t *strip, uint16_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!strip || idx >= strip->max_leds) return;
    uint8_t *p = &strip->pixels[idx * 3];
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

/* Build RMT items for the entire strip and send them. Simple blocking send. */
void led_strip_refresh(led_strip_t *strip, uint32_t timeout_ms)
{
    if (!strip || !strip->inited) return;
    if (!strip->pixels) return;

    /* WS2812 expects GRB ordering per byte */
    const uint32_t T0H = 14; /* ~350ns */
    const uint32_t T0L = 34; /* ~850ns */
    const uint32_t T1H = 34; /* ~850ns */
    const uint32_t T1L = 14; /* ~350ns */

    size_t pixels = strip->max_leds;
    /* 24 bits per LED, one rmt_item per bit */
    size_t items_len = pixels * 24;
    rmt_item32_t *items = calloc(items_len, sizeof(rmt_item32_t));
    if (!items) return;
    size_t idx = 0;
    for (size_t led = 0; led < pixels; ++led) {
        uint8_t r = strip->pixels[led*3 + 0];
        uint8_t g = strip->pixels[led*3 + 1];
        uint8_t b = strip->pixels[led*3 + 2];
        uint8_t data[3] = { g, r, b };
        for (int byte = 0; byte < 3; ++byte) {
            for (int bit = 7; bit >= 0; --bit) {
                bool bit1 = (data[byte] >> bit) & 0x01;
                if (bit1) {
                    items[idx].level0 = 1;
                    items[idx].duration0 = T1H;
                    items[idx].level1 = 0;
                    items[idx].duration1 = T1L;
                } else {
                    items[idx].level0 = 1;
                    items[idx].duration0 = T0H;
                    items[idx].level1 = 0;
                    items[idx].duration1 = T0L;
                }
                idx++;
            }
        }
    }

    rmt_write_items(strip->channel, items, idx, true);
    rmt_wait_tx_done(strip->channel, pdMS_TO_TICKS(timeout_ms));
    free(items);
}
