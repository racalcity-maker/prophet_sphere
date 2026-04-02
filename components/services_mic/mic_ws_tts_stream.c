#include "mic_ws_tts_stream.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/task.h"

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static void ws_account_tts_chunk_result(mic_ws_state_t *state, portMUX_TYPE *lock, esp_err_t err)
{
    portENTER_CRITICAL(lock);
    if (err == ESP_OK) {
        state->tts_chunks_sent++;
    } else {
        state->tts_chunks_dropped++;
    }
    portEXIT_CRITICAL(lock);
}

void mic_ws_tts_stream_flush_tail(mic_ws_state_t *state, portMUX_TYPE *lock)
{
    if (state == NULL || lock == NULL) {
        return;
    }

    uint16_t tail_count = 0U;
    mic_ws_tts_chunk_cb_t cb = NULL;
    void *cb_ctx = NULL;
    bool active = false;

    portENTER_CRITICAL(lock);
    tail_count = state->tts_pcm_tail_count;
    state->tts_pcm_tail_count = 0U;
    cb = state->tts_chunk_cb;
    cb_ctx = state->tts_chunk_cb_ctx;
    active = state->tts_active;
    portEXIT_CRITICAL(lock);

    if (!active || cb == NULL || tail_count == 0U) {
        return;
    }

    esp_err_t err = cb(state->tts_pcm_tail, tail_count, cb_ctx);
    ws_account_tts_chunk_result(state, lock, err);
}

void mic_ws_tts_stream_handle_binary(mic_ws_state_t *state,
                                     portMUX_TYPE *lock,
                                     const uint8_t *data,
                                     size_t len,
                                     const char *log_tag)
{
    if (state == NULL || lock == NULL || data == NULL || len < 2U) {
        return;
    }

    int16_t *chunk = state->tts_pcm_work;
    if (chunk == NULL) {
        return;
    }
    uint16_t used = 0U;
    mic_ws_tts_chunk_cb_t cb = NULL;
    void *cb_ctx = NULL;
    bool active = false;

    portENTER_CRITICAL(lock);
    used = state->tts_pcm_tail_count;
    if (used > WS_TTS_CHUNK_SAMPLES) {
        used = 0U;
    }
    if (used > 0U) {
        memcpy(chunk, state->tts_pcm_tail, (size_t)used * sizeof(int16_t));
    }
    state->tts_pcm_tail_count = 0U;
    cb = state->tts_chunk_cb;
    cb_ctx = state->tts_chunk_cb_ctx;
    active = state->tts_active;
    portEXIT_CRITICAL(lock);

    if (!active || cb == NULL) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    size_t aligned = len & ~((size_t)1U);
    for (size_t i = 0U; i < aligned; i += 2U) {
        int16_t sample = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1U] << 8U));
        chunk[used++] = sample;
        if (used == WS_TTS_CHUNK_SAMPLES) {
            esp_err_t err = cb(chunk, used, cb_ctx);
            ws_account_tts_chunk_result(state, lock, err);
            used = 0U;
        }
    }

    if (used > 0U) {
        portENTER_CRITICAL(lock);
        state->tts_pcm_tail_count = used;
        memcpy(state->tts_pcm_tail, chunk, (size_t)used * sizeof(int16_t));
        portEXIT_CRITICAL(lock);
    }

    bool log_diag = false;
    uint32_t bytes_rx = 0U;
    uint32_t frames_rx = 0U;
    uint32_t chunks_ok = 0U;
    uint32_t chunks_drop = 0U;
    uint32_t sample_rate = 0U;
    portENTER_CRITICAL(lock);
    state->tts_bytes_rx += (uint32_t)aligned;
    state->tts_frames_rx++;
    if (state->tts_last_diag_tick == 0 ||
        (now - state->tts_last_diag_tick) >= ms_to_ticks_min1(1000U)) {
        state->tts_last_diag_tick = now;
        log_diag = true;
        bytes_rx = state->tts_bytes_rx;
        frames_rx = state->tts_frames_rx;
        chunks_ok = state->tts_chunks_sent;
        chunks_drop = state->tts_chunks_dropped;
        sample_rate = state->sample_rate_hz;
    }
    portEXIT_CRITICAL(lock);

    if (log_diag) {
        uint32_t audio_ms = (sample_rate > 0U) ? (bytes_rx * 1000U / (sample_rate * (uint32_t)sizeof(int16_t))) : 0U;
        ESP_LOGD(log_tag,
                 "mic ws tts rx diag frames=%" PRIu32 " bytes=%" PRIu32
                 " audio_ms=%" PRIu32 " chunks_ok=%" PRIu32 " chunks_drop=%" PRIu32,
                 frames_rx,
                 bytes_rx,
                 audio_ms,
                 chunks_ok,
                 chunks_drop);
    }
}
