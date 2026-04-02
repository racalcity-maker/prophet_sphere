#include "audio_service.h"

#include "sdkconfig.h"
#include "app_tasking.h"
#include "audio_asset_registry.h"
#include "audio_output_i2s.h"
#include "audio_task.h"
#include "esp_log.h"
#include "log_tags.h"
#include "service_lifecycle_guard.h"

static const char *TAG = LOG_TAG_AUDIO;

esp_err_t audio_service_init(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "audio init denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "audio service initialized");
    return audio_output_i2s_init();
}

esp_err_t audio_service_start_task(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "audio start denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    return audio_task_start();
}

esp_err_t audio_service_stop_task(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "audio stop denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    return audio_task_stop();
}

esp_err_t audio_service_play_asset(audio_asset_id_t asset_id, uint32_t timeout_ms)
{
    audio_command_t cmd = { 0 };
    cmd.id = AUDIO_CMD_PLAY_ASSET;
    cmd.payload.play_asset.asset_id = (uint32_t)asset_id;
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

esp_err_t audio_service_stop(uint32_t timeout_ms)
{
    audio_command_t cmd = { .id = AUDIO_CMD_STOP };
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

esp_err_t audio_service_set_volume(uint8_t volume, uint32_t timeout_ms)
{
    audio_command_t cmd = { 0 };
    cmd.id = AUDIO_CMD_SET_VOLUME;
    cmd.payload.set_volume.volume = volume;
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

esp_err_t audio_service_set_dynamic_asset_path(audio_asset_id_t slot_id, const char *path)
{
    return audio_asset_registry_set_dynamic_path(slot_id, path);
}

esp_err_t audio_service_clear_dynamic_asset_paths(void)
{
    return audio_asset_registry_clear_dynamic_paths();
}
