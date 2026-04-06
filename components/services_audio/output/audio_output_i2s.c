#include "audio_output_i2s.h"

#include <stdbool.h>
#include "audio_dac_backend.h"
#include "audio_i2s_hal.h"
#include "esp_check.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO_I2S;
static bool s_initialized;

esp_err_t audio_output_i2s_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(audio_i2s_hal_init(), TAG, "I2S HAL init failed");
    ESP_RETURN_ON_ERROR(g_audio_dac_backend.init(), TAG, "DAC backend init failed");

    s_initialized = true;
    ESP_LOGI(TAG, "audio output backend ready (%s)", g_audio_dac_backend.name);
    return ESP_OK;
}

esp_err_t audio_output_i2s_start(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "audio output not initialized");
    ESP_RETURN_ON_ERROR(audio_i2s_hal_start(), TAG, "I2S start failed");
    esp_err_t err = g_audio_dac_backend.start();
    if (err != ESP_OK) {
        (void)audio_i2s_hal_stop();
        return err;
    }
    return ESP_OK;
}

esp_err_t audio_output_i2s_resume_stream(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "audio output not initialized");
    return audio_i2s_hal_start();
}

esp_err_t audio_output_i2s_pause_stream(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "audio output not initialized");
    return audio_i2s_hal_stop();
}

esp_err_t audio_output_i2s_stop(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "audio output not initialized");
    esp_err_t backend_err = g_audio_dac_backend.stop();
    esp_err_t err = audio_i2s_hal_stop();
    if (err != ESP_OK) {
        return err;
    }
    return backend_err;
}

esp_err_t audio_output_i2s_write_mono_pcm16(const int16_t *samples, size_t sample_count, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "audio output not initialized");
    return audio_i2s_hal_write_mono_pcm16(samples, sample_count, timeout_ms);
}
