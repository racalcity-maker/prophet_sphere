#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "mic_ws_client.h"
#include "mic_ws_client_state.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_MIC_WS_ENABLE
#define CONFIG_ORB_MIC_WS_ENABLE 0
#endif
#ifndef CONFIG_ORB_MIC_WS_URL
#define CONFIG_ORB_MIC_WS_URL ""
#endif
#ifndef CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS
#define CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS 1500
#endif
#ifndef CONFIG_ORB_MIC_WS_SEND_TIMEOUT_MS
#define CONFIG_ORB_MIC_WS_SEND_TIMEOUT_MS 200
#endif
#ifndef CONFIG_ORB_MIC_WS_LOG_RESULTS
#define CONFIG_ORB_MIC_WS_LOG_RESULTS 0
#endif
#ifndef CONFIG_ORB_MIC_WS_SEND_RETRY_COUNT
#define CONFIG_ORB_MIC_WS_SEND_RETRY_COUNT 6
#endif
#ifndef CONFIG_ORB_MIC_WS_SEND_RETRY_BACKOFF_MS
#define CONFIG_ORB_MIC_WS_SEND_RETRY_BACKOFF_MS 20
#endif
#ifndef CONFIG_ORB_MIC_WS_TTS_TIMEOUT_MS
#define CONFIG_ORB_MIC_WS_TTS_TIMEOUT_MS 15000
#endif
#ifndef CONFIG_ORB_MIC_WS_TTS_FIRST_CHUNK_TIMEOUT_MS
#define CONFIG_ORB_MIC_WS_TTS_FIRST_CHUNK_TIMEOUT_MS 7000
#endif
#ifndef CONFIG_ORB_MIC_WS_TTS_VOICE
#define CONFIG_ORB_MIC_WS_TTS_VOICE "ruslan"
#endif

#define MIC_WS_RESULT_POLL_MS 10U
#define MIC_WS_SEND_TIMEOUT_MIN_MS 120U
#define MIC_WS_CLIENT_TASK_STACK_BYTES 12288

extern mic_ws_state_t g_mic_ws;
extern portMUX_TYPE g_mic_ws_lock;

TickType_t mic_ws_ms_to_ticks_min1(uint32_t ms);
bool mic_ws_time_reached(TickType_t now, TickType_t deadline);
uint32_t mic_ws_effective_send_timeout_ms(void);
bool mic_ws_wait_connected(esp_websocket_client_handle_t client, uint32_t timeout_ms);

esp_err_t mic_ws_send_start_frame(esp_websocket_client_handle_t client,
                                  uint32_t capture_id,
                                  uint32_t sample_rate_hz);
esp_err_t mic_ws_send_tts_request(esp_websocket_client_handle_t client,
                                  const char *text,
                                  uint32_t sample_rate_hz);

void mic_ws_prepare_kws_session_locked(uint32_t capture_id, uint32_t sample_rate_hz);
void mic_ws_prepare_tts_session_locked(uint32_t sample_rate_hz,
                                       mic_ws_tts_chunk_cb_t chunk_cb,
                                       void *chunk_cb_ctx);
esp_err_t mic_ws_acquire_client(bool block_if_tts_active,
                                bool *out_reuse_connection,
                                esp_websocket_client_handle_t *out_client);
void mic_ws_bind_transport_locked(esp_websocket_client_handle_t client, bool connected);

void mic_ws_websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data);
