#ifndef MIC_SERVICE_H
#define MIC_SERVICE_H

#include <stdint.h>
#include "esp_err.h"
#include "mic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Queue-based and thread-safe frontend.
 * mic_task is the only owner of microphone capture and I2S RX state.
 */
esp_err_t mic_service_init(void);
esp_err_t mic_service_start_task(void);
esp_err_t mic_service_stop_task(void);

esp_err_t mic_service_start_capture(uint32_t capture_id, uint32_t max_capture_ms, uint32_t timeout_ms);
esp_err_t mic_service_stop_capture(uint32_t timeout_ms);
/* Enqueue remote TTS playback on mic task; caller owns audio-side orchestration (PCM stream/background). */
esp_err_t mic_service_play_tts_text(const char *text,
                                    uint32_t stream_timeout_ms,
                                    uint32_t bg_fade_out_ms,
                                    uint32_t timeout_ms);
esp_err_t mic_service_get_status(mic_capture_status_t *out_status);

bool mic_service_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
