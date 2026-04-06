#include "mic_ws_client.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "mic_ws_client_internal.h"

static const char *TAG = LOG_TAG_MIC;

typedef struct {
    bool done;
    bool failed;
    bool connected;
    uint32_t sent;
    uint32_t dropped;
    uint32_t bytes;
    uint32_t frames;
    uint32_t boundary_jumps;
    uint32_t boundary_jump_max;
    TickType_t started_tick;
} mic_ws_tts_snapshot_t;

typedef enum {
    MIC_WS_TTS_TERM_NONE = 0,
    MIC_WS_TTS_TERM_DONE,
    MIC_WS_TTS_TERM_REMOTE_FAILED,
    MIC_WS_TTS_TERM_DISCONNECTED,
    MIC_WS_TTS_TERM_FIRST_CHUNK_TIMEOUT,
    MIC_WS_TTS_TERM_STREAM_TIMEOUT,
} mic_ws_tts_terminal_reason_t;

static void ws_tts_snapshot(mic_ws_tts_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&g_mic_ws_lock);
    out->done = g_mic_ws.tts.tts_done;
    out->failed = g_mic_ws.tts.tts_failed;
    out->connected = g_mic_ws.transport.connected;
    out->sent = g_mic_ws.tts.tts_chunks_sent;
    out->dropped = g_mic_ws.tts.tts_chunks_dropped;
    out->bytes = g_mic_ws.tts.tts_bytes_rx;
    out->frames = g_mic_ws.tts.tts_frames_rx;
    out->boundary_jumps = g_mic_ws.tts.tts_boundary_jump_count;
    out->boundary_jump_max = g_mic_ws.tts.tts_boundary_jump_max;
    out->started_tick = g_mic_ws.tts.tts_started_tick;
    portEXIT_CRITICAL(&g_mic_ws_lock);
}

static mic_ws_tts_terminal_reason_t ws_tts_eval_terminal(const mic_ws_tts_snapshot_t *snapshot,
                                                         TickType_t now_tick,
                                                         TickType_t first_chunk_deadline,
                                                         TickType_t stream_deadline)
{
    if (snapshot == NULL) {
        return MIC_WS_TTS_TERM_REMOTE_FAILED;
    }
    if (snapshot->done) {
        return MIC_WS_TTS_TERM_DONE;
    }
    if (snapshot->failed) {
        return MIC_WS_TTS_TERM_REMOTE_FAILED;
    }
    if (!snapshot->connected) {
        return MIC_WS_TTS_TERM_DISCONNECTED;
    }
    if (snapshot->frames == 0U && mic_ws_time_reached(now_tick, first_chunk_deadline)) {
        return MIC_WS_TTS_TERM_FIRST_CHUNK_TIMEOUT;
    }
    if (mic_ws_time_reached(now_tick, stream_deadline)) {
        return MIC_WS_TTS_TERM_STREAM_TIMEOUT;
    }
    return MIC_WS_TTS_TERM_NONE;
}

esp_err_t mic_ws_client_tts_play(const char *text,
                                 uint32_t sample_rate_hz,
                                 uint32_t timeout_ms,
                                 mic_ws_tts_chunk_cb_t chunk_cb,
                                 void *chunk_cb_ctx)
{
    if (!mic_ws_client_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (text == NULL || text[0] == '\0' || chunk_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0U) {
        timeout_ms = CONFIG_ORB_MIC_WS_TTS_TIMEOUT_MS;
    }
    if (!g_mic_ws.initialized) {
        ESP_RETURN_ON_ERROR(mic_ws_client_init(), TAG, "mic ws init failed");
    }

    esp_websocket_client_handle_t client = NULL;
    bool reuse_connection = false;
    ESP_RETURN_ON_ERROR(mic_ws_acquire_client(false, &reuse_connection, &client),
                        TAG,
                        "mic ws acquire client failed");

    portENTER_CRITICAL(&g_mic_ws_lock);
    mic_ws_bind_transport_locked(client, reuse_connection);
    mic_ws_prepare_tts_session_locked(sample_rate_hz, chunk_cb, chunk_cb_ctx);
    portEXIT_CRITICAL(&g_mic_ws_lock);

    if (!reuse_connection && !mic_ws_wait_connected(client, CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS)) {
        mic_ws_client_abort();
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = mic_ws_send_tts_request(client, text, sample_rate_hz);
    if (err != ESP_OK) {
        mic_ws_client_abort();
        return err;
    }

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t deadline = start_tick + mic_ws_ms_to_ticks_min1(timeout_ms);
    TickType_t first_chunk_deadline = start_tick + mic_ws_ms_to_ticks_min1(CONFIG_ORB_MIC_WS_TTS_FIRST_CHUNK_TIMEOUT_MS);
    for (;;) {
        TickType_t now_tick = xTaskGetTickCount();
        mic_ws_tts_snapshot_t snapshot;
        ws_tts_snapshot(&snapshot);

        mic_ws_tts_terminal_reason_t term =
            ws_tts_eval_terminal(&snapshot, now_tick, first_chunk_deadline, deadline);

        if (term == MIC_WS_TTS_TERM_NONE) {
            vTaskDelay(mic_ws_ms_to_ticks_min1(10U));
            continue;
        }

        if (term == MIC_WS_TTS_TERM_DONE) {
            uint32_t wall_ms = (uint32_t)((xTaskGetTickCount() - snapshot.started_tick) * portTICK_PERIOD_MS);
            uint32_t audio_ms =
                (sample_rate_hz > 0U) ? (snapshot.bytes * 1000U / (sample_rate_hz * (uint32_t)sizeof(int16_t))) : 0U;
            if (snapshot.frames == 0U || snapshot.bytes == 0U) {
                ESP_LOGW(TAG,
                         "mic ws tts done without audio payload wall_ms=%" PRIu32
                         " frames=%" PRIu32 " bytes=%" PRIu32,
                         wall_ms,
                         snapshot.frames,
                         snapshot.bytes);
                mic_ws_client_abort();
                return ESP_ERR_INVALID_RESPONSE;
            }
            ESP_LOGI(TAG,
                     "mic ws tts complete wall_ms=%" PRIu32 " audio_ms=%" PRIu32
                     " frames=%" PRIu32 " chunks_ok=%" PRIu32 " chunks_drop=%" PRIu32
                     " jumps=%" PRIu32 " jump_max=%" PRIu32,
                     wall_ms,
                     audio_ms,
                     snapshot.frames,
                     snapshot.sent,
                     snapshot.dropped,
                     snapshot.boundary_jumps,
                     snapshot.boundary_jump_max);
            mic_ws_client_abort();
            return ESP_OK;
        }

        if (term == MIC_WS_TTS_TERM_FIRST_CHUNK_TIMEOUT) {
            uint32_t wait_ms = (uint32_t)((now_tick - start_tick) * portTICK_PERIOD_MS);
            ESP_LOGW(TAG,
                     "mic ws tts no first audio chunk within %" PRIu32 "ms (waited=%" PRIu32 "ms), abort",
                     (uint32_t)CONFIG_ORB_MIC_WS_TTS_FIRST_CHUNK_TIMEOUT_MS,
                     wait_ms);
            mic_ws_client_abort();
            return ESP_ERR_TIMEOUT;
        }

        if (term == MIC_WS_TTS_TERM_STREAM_TIMEOUT) {
            ESP_LOGW(TAG,
                     "mic ws tts stream timeout: frames=%" PRIu32 " bytes=%" PRIu32
                     " chunks_ok=%" PRIu32 " chunks_drop=%" PRIu32
                     " jumps=%" PRIu32 " jump_max=%" PRIu32,
                     snapshot.frames,
                     snapshot.bytes,
                     snapshot.sent,
                     snapshot.dropped,
                     snapshot.boundary_jumps,
                     snapshot.boundary_jump_max);
            mic_ws_client_abort();
            return ESP_ERR_TIMEOUT;
        }

        ESP_LOGW(TAG,
                 "mic ws tts failed state: reason=%s failed=%d connected=%d frames=%" PRIu32
                 " bytes=%" PRIu32 " chunks_ok=%" PRIu32 " chunks_drop=%" PRIu32
                 " jumps=%" PRIu32 " jump_max=%" PRIu32,
                 (term == MIC_WS_TTS_TERM_DISCONNECTED) ? "disconnected" : "remote_failed",
                 snapshot.failed ? 1 : 0,
                 snapshot.connected ? 1 : 0,
                 snapshot.frames,
                 snapshot.bytes,
                 snapshot.sent,
                 snapshot.dropped,
                 snapshot.boundary_jumps,
                 snapshot.boundary_jump_max);
        mic_ws_client_abort();
        return ESP_FAIL;
    }
}
