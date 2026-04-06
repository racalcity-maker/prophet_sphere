#ifndef MIC_WS_CLIENT_H
#define MIC_WS_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include "app_defs.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional WebSocket transport for remote mic intent inference.
 * mic_task remains the only owner that calls this module.
 */
esp_err_t mic_ws_client_init(void);
void mic_ws_client_deinit(void);
bool mic_ws_client_is_enabled(void);

esp_err_t mic_ws_client_session_start(uint32_t capture_id, uint32_t sample_rate_hz);
esp_err_t mic_ws_client_session_send_pcm16(const int16_t *samples, uint16_t sample_count);
esp_err_t mic_ws_client_session_finish(void);
void mic_ws_client_abort(void);

esp_err_t mic_ws_client_take_result(uint32_t capture_id,
                                    orb_intent_id_t *out_intent,
                                    uint16_t *out_confidence_permille,
                                    uint32_t timeout_ms);

typedef esp_err_t (*mic_ws_tts_chunk_cb_t)(const int16_t *samples, uint16_t sample_count, void *user_ctx);
esp_err_t mic_ws_client_tts_play(const char *text,
                                 uint32_t sample_rate_hz,
                                 uint32_t timeout_ms,
                                 mic_ws_tts_chunk_cb_t chunk_cb,
                                 void *chunk_cb_ctx);

#ifdef __cplusplus
}
#endif

#endif
