#include "led_strip_encoder.h"

#include <stdlib.h>
#include "esp_check.h"

static const char *TAG = "led_encoder";

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t bytes_encoder;
    rmt_encoder_handle_t copy_encoder;
    rmt_symbol_word_t reset_code;
    int state;
} led_strip_rmt_encoder_t;

static size_t led_strip_encode(rmt_encoder_t *encoder,
                               rmt_channel_handle_t channel,
                               const void *primary_data,
                               size_t data_size,
                               rmt_encode_state_t *ret_state)
{
    led_strip_rmt_encoder_t *strip_encoder = __containerof(encoder, led_strip_rmt_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded = 0;

    switch (strip_encoder->state) {
    case 0:
        encoded += strip_encoder->bytes_encoder->encode(
            strip_encoder->bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            strip_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    /* fall through */
    case 1:
        encoded += strip_encoder->copy_encoder->encode(strip_encoder->copy_encoder,
                                                       channel,
                                                       &strip_encoder->reset_code,
                                                       sizeof(strip_encoder->reset_code),
                                                       &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            strip_encoder->state = 0;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        break;
    default:
        strip_encoder->state = 0;
        break;
    }

out:
    *ret_state = state;
    return encoded;
}

static esp_err_t led_strip_del(rmt_encoder_t *encoder)
{
    led_strip_rmt_encoder_t *strip_encoder = __containerof(encoder, led_strip_rmt_encoder_t, base);
    rmt_del_encoder(strip_encoder->bytes_encoder);
    rmt_del_encoder(strip_encoder->copy_encoder);
    free(strip_encoder);
    return ESP_OK;
}

static esp_err_t led_strip_reset(rmt_encoder_t *encoder)
{
    led_strip_rmt_encoder_t *strip_encoder = __containerof(encoder, led_strip_rmt_encoder_t, base);
    rmt_encoder_reset(strip_encoder->bytes_encoder);
    rmt_encoder_reset(strip_encoder->copy_encoder);
    strip_encoder->state = 0;
    return ESP_OK;
}

esp_err_t led_strip_new_rmt_encoder(const led_strip_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder)
{
    ESP_RETURN_ON_FALSE(config != NULL && ret_encoder != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid args");

    led_strip_rmt_encoder_t *strip_encoder = rmt_alloc_encoder_mem(sizeof(led_strip_rmt_encoder_t));
    ESP_RETURN_ON_FALSE(strip_encoder != NULL, ESP_ERR_NO_MEM, TAG, "no memory");

    strip_encoder->base.encode = led_strip_encode;
    strip_encoder->base.del = led_strip_del;
    strip_encoder->base.reset = led_strip_reset;
    strip_encoder->state = 0;

    uint32_t t0h_ticks = (config->resolution_hz * 3U + 9999999U) / 10000000U;
    uint32_t t0l_ticks = (config->resolution_hz * 9U + 9999999U) / 10000000U;
    uint32_t t1h_ticks = (config->resolution_hz * 9U + 9999999U) / 10000000U;
    uint32_t t1l_ticks = (config->resolution_hz * 3U + 9999999U) / 10000000U;
    if (t0h_ticks == 0U) {
        t0h_ticks = 1U;
    }
    if (t0l_ticks == 0U) {
        t0l_ticks = 1U;
    }
    if (t1h_ticks == 0U) {
        t1h_ticks = 1U;
    }
    if (t1l_ticks == 0U) {
        t1l_ticks = 1U;
    }

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 =
            {
                .level0 = 1,
                .duration0 = t0h_ticks, /* 0.3us */
                .level1 = 0,
                .duration1 = t0l_ticks, /* 0.9us */
            },
        .bit1 =
            {
                .level0 = 1,
                .duration0 = t1h_ticks, /* 0.9us */
                .level1 = 0,
                .duration1 = t1l_ticks, /* 0.3us */
            },
        .flags.msb_first = 1,
    };
    esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &strip_encoder->bytes_encoder);
    if (err != ESP_OK) {
        free(strip_encoder);
        return err;
    }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &strip_encoder->copy_encoder);
    if (err != ESP_OK) {
        rmt_del_encoder(strip_encoder->bytes_encoder);
        free(strip_encoder);
        return err;
    }

    uint32_t reset_ticks = (config->resolution_hz * 50U) / 2000000U; /* 50us total split into two halves */
    if (reset_ticks == 0U) {
        reset_ticks = 1U;
    }
    strip_encoder->reset_code = (rmt_symbol_word_t){
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };

    *ret_encoder = &strip_encoder->base;
    return ESP_OK;
}
