#include "mic_i2s_hal.h"

#include <stdbool.h>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "log_tags.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MIC_I2S;
static i2s_chan_handle_t s_rx_channel;
static bool s_initialized;
static bool s_started;

#ifndef CONFIG_ORB_MIC_I2S_PORT
#define CONFIG_ORB_MIC_I2S_PORT 1
#endif
#ifndef CONFIG_ORB_MIC_I2S_BCLK_GPIO
#define CONFIG_ORB_MIC_I2S_BCLK_GPIO 6
#endif
#ifndef CONFIG_ORB_MIC_I2S_WS_GPIO
#define CONFIG_ORB_MIC_I2S_WS_GPIO 7
#endif
#ifndef CONFIG_ORB_MIC_I2S_DIN_GPIO
#define CONFIG_ORB_MIC_I2S_DIN_GPIO 8
#endif
#ifndef CONFIG_ORB_MIC_SAMPLE_RATE_HZ
#define CONFIG_ORB_MIC_SAMPLE_RATE_HZ 16000
#endif
#ifndef CONFIG_ORB_MIC_I2S_DMA_DESC_NUM
#define CONFIG_ORB_MIC_I2S_DMA_DESC_NUM 8
#endif
#ifndef CONFIG_ORB_MIC_I2S_DMA_FRAME_NUM
#define CONFIG_ORB_MIC_I2S_DMA_FRAME_NUM 256
#endif
#ifndef CONFIG_ORB_MIC_I2S_USE_RIGHT_SLOT
#define CONFIG_ORB_MIC_I2S_USE_RIGHT_SLOT 0
#endif

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static TickType_t i2s_read_timeout_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return portMAX_DELAY;
    }
    return ms_to_ticks_min1(timeout_ms);
}

static esp_err_t create_rx_channel(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)CONFIG_ORB_MIC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = (uint32_t)CONFIG_ORB_MIC_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = (uint32_t)CONFIG_ORB_MIC_I2S_DMA_FRAME_NUM;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_channel), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_ORB_MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = CONFIG_ORB_MIC_I2S_BCLK_GPIO,
                .ws = CONFIG_ORB_MIC_I2S_WS_GPIO,
                .dout = I2S_GPIO_UNUSED,
                .din = CONFIG_ORB_MIC_I2S_DIN_GPIO,
            },
    };

#if CONFIG_ORB_MIC_I2S_USE_RIGHT_SLOT
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
#else
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
#endif

    esp_err_t err = i2s_channel_init_std_mode(s_rx_channel, &std_cfg);
    if (err != ESP_OK) {
        (void)i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        return err;
    }
    return ESP_OK;
}

esp_err_t mic_i2s_hal_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(create_rx_channel(), TAG, "create rx channel failed");
    s_initialized = true;
    ESP_LOGI(TAG,
             "I2S RX ready port=%d bclk=%d ws=%d din=%d sr=%d dma_desc=%d dma_frame=%d",
             CONFIG_ORB_MIC_I2S_PORT,
             CONFIG_ORB_MIC_I2S_BCLK_GPIO,
             CONFIG_ORB_MIC_I2S_WS_GPIO,
             CONFIG_ORB_MIC_I2S_DIN_GPIO,
             CONFIG_ORB_MIC_SAMPLE_RATE_HZ,
             CONFIG_ORB_MIC_I2S_DMA_DESC_NUM,
             CONFIG_ORB_MIC_I2S_DMA_FRAME_NUM);
    return ESP_OK;
}

esp_err_t mic_i2s_hal_start(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "I2S RX not initialized");
    if (s_started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_channel), TAG, "i2s_channel_enable failed");
    s_started = true;
    return ESP_OK;
}

esp_err_t mic_i2s_hal_stop(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "I2S RX not initialized");
    if (!s_started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_rx_channel), TAG, "i2s_channel_disable failed");
    s_started = false;
    return ESP_OK;
}

esp_err_t mic_i2s_hal_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_started) {
        (void)mic_i2s_hal_stop();
    }
    if (s_rx_channel != NULL) {
        (void)i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }
    s_initialized = false;
    return ESP_OK;
}

esp_err_t mic_i2s_hal_read_i32(int32_t *samples, size_t sample_count, size_t *out_samples, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized && s_started, ESP_ERR_INVALID_STATE, TAG, "I2S RX not running");
    ESP_RETURN_ON_FALSE(samples != NULL || sample_count == 0, ESP_ERR_INVALID_ARG, TAG, "null sample buffer");
    ESP_RETURN_ON_FALSE(out_samples != NULL, ESP_ERR_INVALID_ARG, TAG, "null out_samples");

    *out_samples = 0U;
    if (sample_count == 0U) {
        return ESP_OK;
    }

    size_t bytes_read = 0U;
    esp_err_t err = i2s_channel_read(
        s_rx_channel, samples, sample_count * sizeof(int32_t), &bytes_read, i2s_read_timeout_ticks(timeout_ms));
    if (err != ESP_OK) {
        return err;
    }

    *out_samples = bytes_read / sizeof(int32_t);
    return ESP_OK;
}
