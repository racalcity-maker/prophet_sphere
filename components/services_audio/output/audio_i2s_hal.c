#include "audio_i2s_hal.h"

#include <stdbool.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "log_tags.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_AUDIO_I2S;
static i2s_chan_handle_t s_tx_channel;
static bool s_initialized;
static bool s_started;
#if !CONFIG_ORB_AUDIO_I2S_MONO
static int16_t s_stereo_tx_buffer[256 * 2];
#endif

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static TickType_t i2s_write_timeout_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return portMAX_DELAY;
    }
    return ms_to_ticks_min1(timeout_ms);
}

static esp_err_t create_tx_channel(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    const uint32_t base_desc_num = (uint32_t)CONFIG_ORB_AUDIO_I2S_DMA_DESC_NUM;
    const uint32_t base_frame_num = (uint32_t)CONFIG_ORB_AUDIO_I2S_DMA_FRAME_NUM;
    uint32_t dma_desc_num = base_desc_num;
    uint32_t dma_frame_num = base_frame_num;
    bool boosted_try = false;
    bool can_fallback_to_base = false;

#if CONFIG_ORB_AUDIO_REAL_MP3_ENABLE
    /*
     * When background playback is active, foreground asset resolution/open can
     * momentarily stall the audio worker. Keep a deeper DMA queue to avoid
     * underrun buzz at MP3 track boundaries.
     */
    const uint32_t min_desc_num = 16U;
    const uint32_t min_frame_num = 512U;
    if (dma_desc_num < min_desc_num || dma_frame_num < min_frame_num) {
        can_fallback_to_base = true;
        boosted_try = true;
        ESP_LOGW(TAG,
                 "raising I2S DMA depth for MP3+BG stability: desc %lu->%lu frame %lu->%lu",
                 (unsigned long)dma_desc_num,
                 (unsigned long)((dma_desc_num < min_desc_num) ? min_desc_num : dma_desc_num),
                 (unsigned long)dma_frame_num,
                 (unsigned long)((dma_frame_num < min_frame_num) ? min_frame_num : dma_frame_num));
        if (dma_desc_num < min_desc_num) {
            dma_desc_num = min_desc_num;
        }
        if (dma_frame_num < min_frame_num) {
            dma_frame_num = min_frame_num;
        }
    }
#endif

    for (;;) {
        chan_cfg.dma_desc_num = dma_desc_num;
        chan_cfg.dma_frame_num = dma_frame_num;
        esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_channel, NULL);
        if (err != ESP_OK) {
            if (err == ESP_ERR_NO_MEM && boosted_try && can_fallback_to_base) {
                ESP_LOGW(TAG,
                         "I2S DMA boost allocation failed, fallback to base desc=%lu frame=%lu",
                         (unsigned long)base_desc_num,
                         (unsigned long)base_frame_num);
                dma_desc_num = base_desc_num;
                dma_frame_num = base_frame_num;
                boosted_try = false;
                continue;
            }
            return err;
        }

        i2s_slot_mode_t slot_mode = CONFIG_ORB_AUDIO_I2S_MONO ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, slot_mode),
            .gpio_cfg =
                {
                    .mclk = I2S_GPIO_UNUSED,
                    .bclk = CONFIG_ORB_AUDIO_I2S_BCLK_GPIO,
                    .ws = CONFIG_ORB_AUDIO_I2S_WS_GPIO,
                    .dout = CONFIG_ORB_AUDIO_I2S_DOUT_GPIO,
                    .din = I2S_GPIO_UNUSED,
                },
        };
        err = i2s_channel_init_std_mode(s_tx_channel, &std_cfg);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        (void)i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        if (err == ESP_ERR_NO_MEM && boosted_try && can_fallback_to_base) {
            ESP_LOGW(TAG,
                     "I2S std init with boosted DMA failed, fallback to base desc=%lu frame=%lu",
                     (unsigned long)base_desc_num,
                     (unsigned long)base_frame_num);
            dma_desc_num = base_desc_num;
            dma_frame_num = base_frame_num;
            boosted_try = false;
            continue;
        }
        return err;
    }
}

static void i2s_prime_silence(size_t sample_count)
{
    int16_t silence[256] = { 0 };
    size_t remaining = sample_count;
    while (remaining > 0U) {
        size_t chunk_samples = remaining;
        if (chunk_samples > (sizeof(silence) / sizeof(silence[0]))) {
            chunk_samples = sizeof(silence) / sizeof(silence[0]);
        }
        size_t bytes_written = 0;
        (void)i2s_channel_write(s_tx_channel,
                                silence,
                                chunk_samples * sizeof(int16_t),
                                &bytes_written,
                                pdMS_TO_TICKS(20));
        if (bytes_written == 0U) {
            break;
        }
        remaining -= bytes_written / sizeof(int16_t);
    }
}

esp_err_t audio_i2s_hal_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(create_tx_channel(), TAG, "create tx channel failed");

    s_initialized = true;
    ESP_LOGI(TAG,
             "I2S TX ready bclk=%d ws=%d dout=%d sr=%d mono=%d dma_desc=%d dma_frame=%d",
             CONFIG_ORB_AUDIO_I2S_BCLK_GPIO,
             CONFIG_ORB_AUDIO_I2S_WS_GPIO,
             CONFIG_ORB_AUDIO_I2S_DOUT_GPIO,
             CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ,
             CONFIG_ORB_AUDIO_I2S_MONO ? 1 : 0,
             CONFIG_ORB_AUDIO_I2S_DMA_DESC_NUM,
             CONFIG_ORB_AUDIO_I2S_DMA_FRAME_NUM);
    return ESP_OK;
}

esp_err_t audio_i2s_hal_start(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "I2S HAL not initialized");
    if (s_started) {
        return ESP_OK;
    }

    /*
     * Recreate TX channel on each start.
     * This force-resets I2S/DMA state and avoids stale transport state
     * after previous stream sessions (live/ws/tts/bg combinations).
     */
    if (s_tx_channel != NULL) {
        (void)i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
    }
    ESP_RETURN_ON_ERROR(create_tx_channel(), TAG, "recreate tx channel failed");

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_channel), TAG, "i2s_channel_enable failed");
    s_started = true;

    /*
     * Fill TX DMA with silence on each start.
     * This prevents replay of stale tail samples from previous stream.
     */
    size_t prime_samples = (size_t)CONFIG_ORB_AUDIO_I2S_DMA_DESC_NUM * (size_t)CONFIG_ORB_AUDIO_I2S_DMA_FRAME_NUM;
    if (prime_samples < 256U) {
        prime_samples = 256U;
    }
    i2s_prime_silence(prime_samples);
    return ESP_OK;
}

esp_err_t audio_i2s_hal_stop(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "I2S HAL not initialized");
    if (!s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_channel), TAG, "i2s_channel_disable failed");
    s_started = false;
    return ESP_OK;
}

esp_err_t audio_i2s_hal_write_mono_pcm16(const int16_t *samples, size_t sample_count, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized && s_started, ESP_ERR_INVALID_STATE, TAG, "I2S HAL not running");
    ESP_RETURN_ON_FALSE(samples != NULL || sample_count == 0, ESP_ERR_INVALID_ARG, TAG, "null samples");

    if (sample_count == 0) {
        return ESP_OK;
    }

#if CONFIG_ORB_AUDIO_I2S_MONO
    size_t offset = 0U;
    while (offset < sample_count) {
        size_t bytes_written = 0U;
        size_t remain_samples = sample_count - offset;
        size_t byte_count = remain_samples * sizeof(int16_t);
        esp_err_t err = i2s_channel_write(s_tx_channel,
                                          &samples[offset],
                                          byte_count,
                                          &bytes_written,
                                          i2s_write_timeout_ticks(timeout_ms));
        if (err == ESP_ERR_TIMEOUT) {
            if (bytes_written == 0U) {
                return ESP_ERR_TIMEOUT;
            }
        } else if (err != ESP_OK) {
            return err;
        }
        if (bytes_written == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        size_t frames_written = bytes_written / sizeof(int16_t);
        if (frames_written == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        offset += frames_written;
    }
    return ESP_OK;
#else
    size_t offset = 0;
    while (offset < sample_count) {
        size_t remain = sample_count - offset;
        size_t chunk = remain;
        if (chunk > 256) {
            chunk = 256;
        }

        for (size_t i = 0; i < chunk; ++i) {
            int16_t s = samples[offset + i];
            s_stereo_tx_buffer[(i * 2) + 0] = s;
            s_stereo_tx_buffer[(i * 2) + 1] = s;
        }

        size_t bytes_written = 0;
        size_t byte_count = chunk * 2U * sizeof(int16_t);
        esp_err_t err = i2s_channel_write(
            s_tx_channel, s_stereo_tx_buffer, byte_count, &bytes_written, i2s_write_timeout_ticks(timeout_ms));
        if (err == ESP_ERR_TIMEOUT) {
            if (bytes_written == 0U) {
                return ESP_ERR_TIMEOUT;
            }
        } else if (err != ESP_OK) {
            return err;
        }
        if (bytes_written == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        size_t frames_written = bytes_written / (2U * sizeof(int16_t));
        if (frames_written == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        offset += frames_written;
    }
    return ESP_OK;
#endif
}
