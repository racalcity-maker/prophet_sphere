#include "mic_service.h"

#include "app_tasking.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "log_tags.h"
#include "mic_task.h"
#include "service_lifecycle_guard.h"
#include <stdio.h>

static const char *TAG = LOG_TAG_MIC;

#ifndef CONFIG_ORB_ENABLE_MIC
#define CONFIG_ORB_ENABLE_MIC 0
#endif
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

static bool s_initialized;

esp_err_t mic_service_init(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "mic init denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_initialized) {
        return ESP_OK;
    }

    if (!CONFIG_ORB_ENABLE_MIC) {
        ESP_LOGW(TAG, "microphone service disabled by config");
        s_initialized = true;
        return ESP_OK;
    }

    if (app_tasking_get_mic_cmd_queue() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_initialized = true;

    ESP_LOGI(TAG,
             "microphone service initialized (I2S%d bclk=%d ws=%d din=%d sr=%d)",
             CONFIG_ORB_MIC_I2S_PORT,
             CONFIG_ORB_MIC_I2S_BCLK_GPIO,
             CONFIG_ORB_MIC_I2S_WS_GPIO,
             CONFIG_ORB_MIC_I2S_DIN_GPIO,
             CONFIG_ORB_MIC_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t mic_service_start_task(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "mic start denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_initialized) {
        esp_err_t err = mic_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!CONFIG_ORB_ENABLE_MIC) {
        return ESP_OK;
    }
    return mic_task_start(app_tasking_get_mic_cmd_queue());
}

esp_err_t mic_service_stop_task(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "mic stop denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CONFIG_ORB_ENABLE_MIC) {
        return ESP_OK;
    }
    return mic_task_stop();
}

esp_err_t mic_service_start_capture(uint32_t capture_id, uint32_t max_capture_ms, uint32_t timeout_ms)
{
    if (!CONFIG_ORB_ENABLE_MIC) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    mic_command_t cmd = { 0 };
    cmd.id = MIC_CMD_START_CAPTURE;
    cmd.payload.start_capture.capture_id = capture_id;
    cmd.payload.start_capture.max_capture_ms = max_capture_ms;
    return app_tasking_send_mic_command(&cmd, timeout_ms);
}

esp_err_t mic_service_stop_capture(uint32_t timeout_ms)
{
    if (!CONFIG_ORB_ENABLE_MIC) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    mic_command_t cmd = { .id = MIC_CMD_STOP_CAPTURE };
    return app_tasking_send_mic_command(&cmd, timeout_ms);
}

esp_err_t mic_service_play_tts_text(const char *text, uint32_t stream_timeout_ms, uint32_t timeout_ms)
{
    if (!CONFIG_ORB_ENABLE_MIC) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    mic_command_t cmd = { 0 };
    cmd.id = MIC_CMD_TTS_PLAY_TEXT;
    (void)snprintf(cmd.payload.tts_play.text,
                   sizeof(cmd.payload.tts_play.text),
                   "%s",
                   text);
    cmd.payload.tts_play.timeout_ms = stream_timeout_ms;
    audio_command_t audio_cmd = { .id = AUDIO_CMD_PCM_STREAM_START };
    esp_err_t audio_err = app_tasking_send_audio_command(&audio_cmd, timeout_ms);
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to start audio pcm stream for tts: %s", esp_err_to_name(audio_err));
        return audio_err;
    }

    esp_err_t mic_err = app_tasking_send_mic_command(&cmd, timeout_ms);
    if (mic_err != ESP_OK) {
        audio_command_t stop_cmd = { .id = AUDIO_CMD_PCM_STREAM_STOP };
        esp_err_t rollback_err = app_tasking_send_audio_command(&stop_cmd, timeout_ms);
        if (rollback_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "mic tts command failed (%s), and pcm rollback failed: %s",
                     esp_err_to_name(mic_err),
                     esp_err_to_name(rollback_err));
        } else {
            ESP_LOGW(TAG, "mic tts command failed (%s), rolled back pcm stream", esp_err_to_name(mic_err));
        }
        return mic_err;
    }

    return ESP_OK;
}

esp_err_t mic_service_get_status(mic_capture_status_t *out_status)
{
    if (!CONFIG_ORB_ENABLE_MIC) {
        if (out_status != NULL) {
            *out_status = (mic_capture_status_t){ 0 };
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    return mic_task_get_status(out_status);
}

bool mic_service_is_enabled(void)
{
    return CONFIG_ORB_ENABLE_MIC;
}
