#include "app_media_gateway.h"

#include <limits.h>
#include "sdkconfig.h"

uint32_t app_media_gateway_queue_timeout_ms(void)
{
    return (uint32_t)CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
}

esp_err_t app_media_gateway_send_audio_command(const audio_command_t *cmd, uint32_t timeout_ms)
{
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return app_tasking_send_audio_command(cmd, timeout_ms);
}

esp_err_t app_media_gateway_send_led_command(const led_command_t *cmd, uint32_t timeout_ms)
{
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return app_tasking_send_led_command(cmd, timeout_ms);
}

esp_err_t app_media_gateway_send_mic_command(const mic_command_t *cmd, uint32_t timeout_ms)
{
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return app_tasking_send_mic_command(cmd, timeout_ms);
}

esp_err_t app_media_gateway_send_audio_pcm_chunk_copy(const int16_t *samples, size_t sample_count, uint32_t timeout_ms)
{
    if (sample_count > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return app_tasking_send_audio_pcm_chunk_copy(samples, (uint16_t)sample_count, timeout_ms);
}
