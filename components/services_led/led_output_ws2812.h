#ifndef LED_OUTPUT_WS2812_H
#define LED_OUTPUT_WS2812_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t led_output_ws2812_init(void);
esp_err_t led_output_ws2812_write_grb(const uint8_t *grb_data, size_t length_bytes, uint32_t timeout_ms);
esp_err_t led_output_ws2812_clear(uint32_t timeout_ms);

#endif
