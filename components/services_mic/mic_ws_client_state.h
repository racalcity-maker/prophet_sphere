#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_tasking.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "mic_ws_client.h"
#include "orb_intents.h"

#define WS_TTS_CHUNK_SAMPLES AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES
#define WS_RX_BUF_BYTES 224

typedef enum {
    MIC_WS_MODE_NONE = 0,
    MIC_WS_MODE_KWS,
    MIC_WS_MODE_TTS,
} mic_ws_mode_t;

typedef struct {
    esp_websocket_client_handle_t client;
    bool connected;
    char rx_buf[WS_RX_BUF_BYTES];
    size_t rx_len;
} mic_ws_transport_state_t;

typedef struct {
    bool session_active;
    bool start_sent;
    uint32_t active_capture_id;
    uint32_t sample_rate_hz;
    bool result_ready;
    uint32_t result_capture_id;
    orb_intent_id_t result_intent;
    uint16_t result_conf_permille;
} mic_ws_kws_session_state_t;

typedef struct {
    bool tts_active;
    bool tts_done;
    bool tts_failed;
    mic_ws_tts_chunk_cb_t tts_chunk_cb;
    void *tts_chunk_cb_ctx;
    uint32_t tts_chunks_sent;
    uint32_t tts_chunks_dropped;
    uint32_t tts_bytes_rx;
    uint32_t tts_frames_rx;
    uint32_t tts_boundary_jump_count;
    uint32_t tts_boundary_jump_max;
    bool tts_prev_sample_valid;
    int16_t tts_prev_sample;
    TickType_t tts_started_tick;
    TickType_t tts_last_diag_tick;
    uint16_t tts_pcm_tail_count;
    int16_t tts_pcm_tail[WS_TTS_CHUNK_SAMPLES];
    int16_t tts_pcm_work[WS_TTS_CHUNK_SAMPLES];
    uint32_t sample_rate_hz;
} mic_ws_tts_stream_state_t;

typedef struct {
    bool initialized;
    mic_ws_mode_t mode;
    mic_ws_transport_state_t transport;
    mic_ws_kws_session_state_t kws;
    mic_ws_tts_stream_state_t tts;
} mic_ws_state_t;
