#include "mic_ws_tts_stream.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/task.h"

#define WS_TTS_BOUNDARY_JUMP_THRESHOLD 5000U

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static void ws_account_tts_chunk_result(mic_ws_state_t *state, portMUX_TYPE *lock, esp_err_t err)
{
    portENTER_CRITICAL(lock);
    if (err == ESP_OK) {
        state->tts.tts_chunks_sent++;
    } else {
        state->tts.tts_chunks_dropped++;
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
    tail_count = state->tts.tts_pcm_tail_count;
    state->tts.tts_pcm_tail_count = 0U;
    cb = state->tts.tts_chunk_cb;
    cb_ctx = state->tts.tts_chunk_cb_ctx;
    active = state->tts.tts_active;
    portEXIT_CRITICAL(lock);

    if (!active || cb == NULL || tail_count == 0U) {
        return;
    }

    esp_err_t err = cb(state->tts.tts_pcm_tail, tail_count, cb_ctx);
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

    int16_t *chunk = state->tts.tts_pcm_work;
    if (chunk == NULL) {
        return;
    }
    int16_t first_sample = 0;
    int16_t last_sample = 0;
    bool got_sample = false;
    uint16_t used = 0U;
    mic_ws_tts_chunk_cb_t cb = NULL;
    void *cb_ctx = NULL;
    bool active = false;

    portENTER_CRITICAL(lock);
    used = state->tts.tts_pcm_tail_count;
    if (used > WS_TTS_CHUNK_SAMPLES) {
        used = 0U;
    }
    if (used > 0U) {
        memcpy(chunk, state->tts.tts_pcm_tail, (size_t)used * sizeof(int16_t));
    }
    state->tts.tts_pcm_tail_count = 0U;
    cb = state->tts.tts_chunk_cb;
    cb_ctx = state->tts.tts_chunk_cb_ctx;
    active = state->tts.tts_active;
    portEXIT_CRITICAL(lock);

    if (!active || cb == NULL) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    size_t aligned = len & ~((size_t)1U);
    for (size_t i = 0U; i < aligned; i += 2U) {
        int16_t sample = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1U] << 8U));
        if (!got_sample) {
            first_sample = sample;
            got_sample = true;
        }
        last_sample = sample;
        chunk[used++] = sample;
        if (used == WS_TTS_CHUNK_SAMPLES) {
            esp_err_t err = cb(chunk, used, cb_ctx);
            ws_account_tts_chunk_result(state, lock, err);
            used = 0U;
        }
    }

    if (used > 0U) {
        portENTER_CRITICAL(lock);
        state->tts.tts_pcm_tail_count = used;
        memcpy(state->tts.tts_pcm_tail, chunk, (size_t)used * sizeof(int16_t));
        portEXIT_CRITICAL(lock);
    }

    bool log_diag = false;
    uint32_t bytes_rx = 0U;
    uint32_t frames_rx = 0U;
    uint32_t chunks_ok = 0U;
    uint32_t chunks_drop = 0U;
    uint32_t jumps = 0U;
    uint32_t jump_max = 0U;
    uint32_t sample_rate = 0U;
    portENTER_CRITICAL(lock);
    if (got_sample) {
        if (state->tts.tts_prev_sample_valid) {
            int32_t jump = (int32_t)first_sample - (int32_t)state->tts.tts_prev_sample;
            if (jump < 0) {
                jump = -jump;
            }
            uint32_t abs_jump = (uint32_t)jump;
            if (abs_jump > state->tts.tts_boundary_jump_max) {
                state->tts.tts_boundary_jump_max = abs_jump;
            }
            if (abs_jump >= WS_TTS_BOUNDARY_JUMP_THRESHOLD) {
                state->tts.tts_boundary_jump_count++;
            }
        }
        state->tts.tts_prev_sample = last_sample;
        state->tts.tts_prev_sample_valid = true;
    }
    state->tts.tts_bytes_rx += (uint32_t)aligned;
    state->tts.tts_frames_rx++;
    if (state->tts.tts_last_diag_tick == 0 ||
        (now - state->tts.tts_last_diag_tick) >= ms_to_ticks_min1(1000U)) {
        state->tts.tts_last_diag_tick = now;
        log_diag = true;
        bytes_rx = state->tts.tts_bytes_rx;
        frames_rx = state->tts.tts_frames_rx;
        chunks_ok = state->tts.tts_chunks_sent;
        chunks_drop = state->tts.tts_chunks_dropped;
        jumps = state->tts.tts_boundary_jump_count;
        jump_max = state->tts.tts_boundary_jump_max;
        sample_rate = state->tts.sample_rate_hz;
    }
    portEXIT_CRITICAL(lock);

    if (log_diag) {
        uint32_t audio_ms = (sample_rate > 0U) ? (bytes_rx * 1000U / (sample_rate * (uint32_t)sizeof(int16_t))) : 0U;
        ESP_LOGD(log_tag,
                 "mic ws tts rx diag frames=%" PRIu32 " bytes=%" PRIu32
                 " audio_ms=%" PRIu32 " chunks_ok=%" PRIu32 " chunks_drop=%" PRIu32
                 " jumps=%" PRIu32 " jump_max=%" PRIu32,
                 frames_rx,
                 bytes_rx,
                 audio_ms,
                 chunks_ok,
                 chunks_drop,
                 jumps,
                 jump_max);
    }
}
