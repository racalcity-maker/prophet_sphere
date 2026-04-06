#include "led_output_ws2812.h"

#include <stdbool.h>
#include <string.h>
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp_pins.h"
#include "led_strip_encoder.h"
#include "log_tags.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_LED;

#define ORB_LED_PIXEL_COUNT ((size_t)(CONFIG_ORB_LED_MATRIX_WIDTH * CONFIG_ORB_LED_MATRIX_HEIGHT))
#define ORB_LED_BYTES_PER_PIXEL 3U
#define ORB_LED_BUFFER_BYTES (ORB_LED_PIXEL_COUNT * ORB_LED_BYTES_PER_PIXEL)

static rmt_channel_handle_t s_led_channel;
static rmt_encoder_handle_t s_led_encoder;
static bool s_initialized;
static uint8_t s_clear_buffer[ORB_LED_BUFFER_BYTES];

esp_err_t led_output_ws2812_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

#if !CONFIG_ORB_LED_ENABLE_OUTPUT
    ESP_LOGW(TAG, "WS2812 output disabled by config");
    s_initialized = true;
    return ESP_OK;
#else
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = BSP_LED_DATA_GPIO,
        .mem_block_symbols = CONFIG_ORB_LED_RMT_MEM_BLOCK_SYMBOLS,
        .resolution_hz = CONFIG_ORB_LED_RMT_RESOLUTION_HZ,
        .trans_queue_depth = CONFIG_ORB_LED_RMT_TX_QUEUE_DEPTH,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_led_channel), TAG, "rmt_new_tx_channel failed");

    led_strip_encoder_config_t enc_cfg = {
        .resolution_hz = CONFIG_ORB_LED_RMT_RESOLUTION_HZ,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_encoder(&enc_cfg, &s_led_encoder), TAG, "led encoder create failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_channel), TAG, "rmt_enable failed");

    memset(s_clear_buffer, 0, sizeof(s_clear_buffer));
    s_initialized = true;
    ESP_LOGI(TAG,
             "WS2812 backend ready gpio=%d leds=%u rmt=%dHz",
             BSP_LED_DATA_GPIO,
             (unsigned)ORB_LED_PIXEL_COUNT,
             CONFIG_ORB_LED_RMT_RESOLUTION_HZ);
    return ESP_OK;
#endif
}

esp_err_t led_output_ws2812_write_grb(const uint8_t *grb_data, size_t length_bytes, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "backend not initialized");
    ESP_RETURN_ON_FALSE(grb_data != NULL, ESP_ERR_INVALID_ARG, TAG, "null buffer");

#if !CONFIG_ORB_LED_ENABLE_OUTPUT
    (void)length_bytes;
    (void)timeout_ms;
    return ESP_OK;
#else
    ESP_RETURN_ON_FALSE(length_bytes == ORB_LED_BUFFER_BYTES, ESP_ERR_INVALID_SIZE, TAG, "invalid GRB length");
    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    ESP_RETURN_ON_ERROR(rmt_transmit(s_led_channel, s_led_encoder, grb_data, length_bytes, &tx_cfg), TAG, "rmt_transmit failed");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(s_led_channel, (int)timeout_ms), TAG, "rmt wait timeout");
    return ESP_OK;
#endif
}

esp_err_t led_output_ws2812_clear(uint32_t timeout_ms)
{
    return led_output_ws2812_write_grb(s_clear_buffer, sizeof(s_clear_buffer), timeout_ms);
}
