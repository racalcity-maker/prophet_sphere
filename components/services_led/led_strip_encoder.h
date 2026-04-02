#ifndef LED_STRIP_ENCODER_H
#define LED_STRIP_ENCODER_H

#include <stdint.h>
#include "driver/rmt_encoder.h"
#include "esp_err.h"

typedef struct {
    uint32_t resolution_hz;
} led_strip_encoder_config_t;

esp_err_t led_strip_new_rmt_encoder(const led_strip_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder);

#endif
