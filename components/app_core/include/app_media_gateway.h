#ifndef APP_MEDIA_GATEWAY_H
#define APP_MEDIA_GATEWAY_H

#include <stddef.h>
#include <stdint.h>
#include "app_tasking.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t app_media_gateway_queue_timeout_ms(void);

esp_err_t app_media_gateway_send_audio_command(const audio_command_t *cmd, uint32_t timeout_ms);
esp_err_t app_media_gateway_send_led_command(const led_command_t *cmd, uint32_t timeout_ms);
esp_err_t app_media_gateway_send_mic_command(const mic_command_t *cmd, uint32_t timeout_ms);
esp_err_t app_media_gateway_send_audio_pcm_chunk_copy(const int16_t *samples, size_t sample_count, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
