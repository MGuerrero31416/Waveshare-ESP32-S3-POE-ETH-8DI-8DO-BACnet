#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LED_STRIP_WS2812 = 0 } led_strip_model_t;

typedef struct {
    uint16_t max_leds;
    gpio_num_t gpio_num;
    led_strip_model_t led_model;
} led_strip_config_t;

typedef struct led_strip_s led_strip_t;

/* Create a new RMT-backed led_strip device. Returns NULL on failure. */
led_strip_t* led_strip_new_rmt_device(const led_strip_config_t *config);
void led_strip_init(led_strip_t *strip);
void led_strip_clear(led_strip_t *strip);
void led_strip_set_pixel(led_strip_t *strip, uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
void led_strip_refresh(led_strip_t *strip, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
