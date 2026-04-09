#include "rest_api_talk_internal.h"
#include "rest_api_talk_live_buffer.h"
#include "rest_api_talk_live_postfx.h"
#include "rest_api_talk_live_runtime.h"

#if ORB_TALK_WS_ENABLED
#include <inttypes.h>
#include <string.h>
#endif
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_tags.h"

#if ORB_TALK_WS_ENABLED
static const char *TAG = LOG_TAG_REST;
#endif

#if ORB_TALK_WS_ENABLED
typedef struct {
    SemaphoreHandle_t lock;
    esp_timer_handle_t watchdog;
    bool websocket_open;
    bool pcm_stream_active;
    int sockfd;
    int64_t last_chunk_ms;
    talk_live_buffer_t ring;
    uint32_t rx_chunks;
    uint32_t rx_samples;
    int64_t rx_diag_last_ms;
    TaskHandle_t feeder_task;
    bool feeder_stack_psram;
    talk_live_postfx_state_t postfx;
} talk_live_state_t;

static talk_live_state_t s_live = {
    .lock = NULL,
    .watchdog = NULL,
    .websocket_open = false,
    .pcm_stream_active = false,
    .sockfd = -1,
    .last_chunk_ms = 0,
    .ring = {
        .samples = NULL,
        .capacity_samples = 0U,
        .write_pos = 0U,
        .read_pos = 0U,
        .count = 0U,
        .dropped_samples = 0U,
    },
    .rx_chunks = 0U,
    .rx_samples = 0U,
    .rx_diag_last_ms = 0,
    .feeder_task = NULL,
    .feeder_stack_psram = false,
    .postfx = {
        .has_last_sample = false,
        .last_sample = 0,
        .dc_prev_x = 0,
        .dc_prev_y = 0,
    },
};

static esp_err_t talk_live_lock(void)
{
    if (s_live.lock == NULL) {
        s_live.lock = xSemaphoreCreateMutex();
        if (s_live.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    if (ticks == 0) {
        ticks = 1;
    }
    if (xSemaphoreTake(s_live.lock, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void talk_live_unlock(void)
{
    if (s_live.lock != NULL) {
        xSemaphoreGive(s_live.lock);
    }
}

static esp_err_t talk_live_send_pcm_start(uint32_t timeout_ms)
{
    audio_command_t cmd = { .id = AUDIO_CMD_PCM_STREAM_START };
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

static esp_err_t talk_live_send_pcm_stop(uint32_t timeout_ms)
{
    audio_command_t cmd = { .id = AUDIO_CMD_PCM_STREAM_STOP };
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

static esp_err_t talk_live_send_pcm_chunk(const int16_t *samples, uint16_t sample_count, uint32_t timeout_ms)
{
    if (samples == NULL || sample_count == 0U || sample_count > AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_command_t cmd = { 0 };
    cmd.id = AUDIO_CMD_PCM_STREAM_CHUNK;
    cmd.payload.pcm_stream_chunk.sample_count = sample_count;
    (void)memcpy(cmd.payload.pcm_stream_chunk.samples,
                 samples,
                 (size_t)sample_count * sizeof(int16_t));
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

static void talk_live_feeder_task(void *arg)
{
    (void)arg;
    int16_t *chunk = (int16_t *)heap_caps_malloc((size_t)AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES * sizeof(int16_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        chunk = (int16_t *)heap_caps_malloc((size_t)AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES * sizeof(int16_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (chunk == NULL) {
        ESP_LOGE(TAG, "talk_live_feed chunk buffer alloc failed");
        if (talk_live_lock() == ESP_OK) {
            s_live.feeder_task = NULL;
            s_live.feeder_stack_psram = false;
            talk_live_unlock();
        }
        vTaskDelete(NULL);
        return;
    }
    TickType_t idle_ticks = pdMS_TO_TICKS(TALK_LIVE_FEED_IDLE_MS);
    if (idle_ticks == 0) {
        idle_ticks = 1;
    }

    for (;;) {
        bool need_start = false;
        bool need_stop = false;
        uint16_t chunk_samples = 0U;
        uint32_t buffered_samples = 0U;

        if (talk_live_lock() != ESP_OK) {
            vTaskDelay(idle_ticks);
            continue;
        }

        if (s_live.websocket_open && !s_live.pcm_stream_active && s_live.ring.count >= TALK_LIVE_PREBUFFER_SAMPLES) {
            s_live.pcm_stream_active = true;
            need_start = true;
            buffered_samples = s_live.ring.count;
        }

        if (s_live.pcm_stream_active) {
            chunk_samples = talk_live_buffer_pop(&s_live.ring, chunk, AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES);
            if (chunk_samples == 0U && !s_live.websocket_open) {
                s_live.pcm_stream_active = false;
                need_stop = true;
            }
        }

        talk_live_unlock();

        if (need_start) {
            esp_err_t start_err = talk_live_send_pcm_start(talk_request_timeout_ms());
            if (start_err != ESP_OK) {
                ESP_LOGW(TAG, "live feeder pcm start failed: %s", esp_err_to_name(start_err));
                if (talk_live_lock() == ESP_OK) {
                    s_live.websocket_open = false;
                    s_live.pcm_stream_active = false;
                    s_live.sockfd = -1;
                    talk_live_buffer_reset(&s_live.ring);
                    talk_live_postfx_reset(&s_live.postfx);
                    talk_live_unlock();
                }
            } else {
                uint32_t buffered_ms = (uint32_t)(((uint64_t)buffered_samples * 1000ULL) / TALK_LIVE_SAMPLE_RATE_HZ);
                ESP_LOGI(TAG, "live feeder start playback buffered=%" PRIu32 " samples (%" PRIu32 " ms)",
                         buffered_samples,
                         buffered_ms);
            }
        }

        if (chunk_samples > 0U) {
            esp_err_t chunk_err = talk_live_send_pcm_chunk(chunk, chunk_samples, talk_request_timeout_ms());
            if (chunk_err != ESP_OK) {
                ESP_LOGW(TAG, "live feeder chunk send failed: %s", esp_err_to_name(chunk_err));
            }
        } else if (!need_start && !need_stop) {
            vTaskDelay(idle_ticks);
        }

        if (need_stop) {
            esp_err_t stop_err = talk_live_send_pcm_stop(talk_request_timeout_ms());
            if (stop_err != ESP_OK) {
                ESP_LOGW(TAG, "live feeder pcm stop failed: %s", esp_err_to_name(stop_err));
            } else {
                ESP_LOGI(TAG, "live feeder playback stopped");
            }
        }

        /* Yield CPU each loop but don't add fixed delay that throttles stream draining. */
        taskYIELD();
    }
}

static void talk_live_watchdog_cb(void *arg)
{
    (void)arg;
    if (s_live.lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_live.lock, 0) != pdTRUE) {
        return;
    }

    bool should_stop = false;
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    if (s_live.websocket_open && s_live.last_chunk_ms > 0 &&
        (now_ms - s_live.last_chunk_ms) > (int64_t)TALK_LIVE_INACTIVITY_TIMEOUT_MS) {
        should_stop = s_live.pcm_stream_active;
        s_live.pcm_stream_active = false;
        s_live.websocket_open = false;
        s_live.sockfd = -1;
        talk_live_buffer_reset(&s_live.ring);
        talk_live_postfx_reset(&s_live.postfx);
        s_live.ring.dropped_samples = 0U;
    }
    xSemaphoreGive(s_live.lock);

    if (should_stop) {
        esp_err_t err = talk_live_send_pcm_stop(0U);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "live watchdog stop failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "live watchdog stop: no chunks for %" PRIu32 " ms", (uint32_t)TALK_LIVE_INACTIVITY_TIMEOUT_MS);
        }
    }
}
#endif

esp_err_t talk_live_init(void)
{
#if !ORB_TALK_WS_ENABLED
    return ESP_OK;
#else
    if (talk_live_lock() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    talk_live_unlock();

    ESP_RETURN_ON_ERROR(talk_live_runtime_ensure_watchdog(&s_live.watchdog,
                                                          talk_live_watchdog_cb,
                                                          "talk_live_wd",
                                                          TALK_LIVE_WATCHDOG_PERIOD_MS),
                        TAG,
                        "live watchdog start failed");

    if (s_live.ring.samples == NULL) {
        s_live.ring.capacity_samples = TALK_LIVE_RING_CAPACITY_SAMPLES;
        ESP_RETURN_ON_ERROR(talk_live_runtime_alloc_ring_samples(&s_live.ring.samples, s_live.ring.capacity_samples),
                            TAG,
                            "live ring alloc failed");
        talk_live_buffer_reset(&s_live.ring);
        talk_live_postfx_reset(&s_live.postfx);
        s_live.ring.dropped_samples = 0U;
    }

    ESP_RETURN_ON_ERROR(talk_live_runtime_ensure_feeder_task(&s_live.feeder_task,
                                                             &s_live.feeder_stack_psram,
                                                             talk_live_feeder_task,
                                                             "talk_live_feed",
                                                             TALK_LIVE_FEED_TASK_STACK,
                                                             TALK_LIVE_FEED_TASK_PRIORITY),
                        TAG,
                        "live feeder start failed");
    return ESP_OK;
#endif
}

esp_err_t talk_live_snapshot(bool *open_out, bool *stream_out)
{
    if (open_out == NULL || stream_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if !ORB_TALK_WS_ENABLED
    *open_out = false;
    *stream_out = false;
    return ESP_OK;
#else
    ESP_RETURN_ON_ERROR(talk_live_lock(), TAG, "live lock failed");
    *open_out = s_live.websocket_open;
    *stream_out = s_live.pcm_stream_active;
    talk_live_unlock();
    return ESP_OK;
#endif
}

#if ORB_TALK_WS_ENABLED
esp_err_t talk_live_stop_if_owner(int sockfd, bool close_socket_state)
{
    bool should_stop = false;
    ESP_RETURN_ON_ERROR(talk_live_lock(), TAG, "live lock failed");
    if (s_live.sockfd == sockfd || s_live.sockfd < 0) {
        should_stop = s_live.pcm_stream_active;
        s_live.pcm_stream_active = false;
        talk_live_buffer_reset(&s_live.ring);
        talk_live_postfx_reset(&s_live.postfx);
        if (close_socket_state) {
            s_live.websocket_open = false;
            s_live.sockfd = -1;
        }
    }
    talk_live_unlock();

    if (should_stop) {
        esp_err_t err = talk_live_send_pcm_stop(talk_request_timeout_ms());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "live pcm stop failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t talk_live_mark_open(int sockfd)
{
    ESP_RETURN_ON_ERROR(talk_live_lock(), TAG, "live lock failed");
    if (s_live.websocket_open || s_live.sockfd >= 0) {
        talk_live_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_live.websocket_open = true;
    s_live.pcm_stream_active = false;
    s_live.sockfd = sockfd;
    s_live.last_chunk_ms = esp_timer_get_time() / 1000LL;
    s_live.ring.dropped_samples = 0U;
    s_live.rx_chunks = 0U;
    s_live.rx_samples = 0U;
    s_live.rx_diag_last_ms = 0;
    talk_live_buffer_reset(&s_live.ring);
    talk_live_postfx_reset(&s_live.postfx);
    talk_live_unlock();
    return ESP_OK;
}

esp_err_t talk_live_accept_chunk_owner(int sockfd)
{
    ESP_RETURN_ON_ERROR(talk_live_lock(), TAG, "live lock failed");
    if (!s_live.websocket_open || s_live.sockfd != sockfd) {
        talk_live_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_live.last_chunk_ms = esp_timer_get_time() / 1000LL;
    talk_live_unlock();
    return ESP_OK;
}

esp_err_t talk_live_send_pcm_chunk_bytes(uint8_t *data, size_t len)
{
    if (data == NULL || len < sizeof(int16_t)) {
        return ESP_OK;
    }

    size_t even_len = len & ~(size_t)1U;
    if (even_len == 0) {
        return ESP_OK;
    }
    int16_t *in_samples = (int16_t *)data;
    uint32_t sample_total = (uint32_t)(even_len / sizeof(int16_t));
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    bool emit_diag = false;
    uint32_t diag_chunks = 0U;
    uint32_t diag_samples = 0U;
    int16_t min_v = 32767;
    int16_t max_v = -32768;
    uint64_t sum_abs = 0U;
    uint32_t abs_avg = 0U;

    ESP_RETURN_ON_ERROR(talk_live_lock(), TAG, "live lock failed");
    talk_live_postfx_apply_inplace(&s_live.postfx, in_samples, sample_total);
    uint32_t accepted = talk_live_buffer_push(&s_live.ring, in_samples, sample_total);
    uint32_t dropped = sample_total - accepted;
    uint32_t dropped_total = s_live.ring.dropped_samples;
    s_live.rx_chunks++;
    s_live.rx_samples += sample_total;
    if ((now_ms - s_live.rx_diag_last_ms) >= 1000LL) {
        s_live.rx_diag_last_ms = now_ms;
        emit_diag = true;
        diag_chunks = s_live.rx_chunks;
        diag_samples = s_live.rx_samples;
    }
    for (uint32_t i = 0U; i < sample_total; ++i) {
        int16_t v = in_samples[i];
        if (v < min_v) {
            min_v = v;
        }
        if (v > max_v) {
            max_v = v;
        }
        int32_t iv = (int32_t)v;
        sum_abs += (uint64_t)((iv < 0) ? -iv : iv);
    }
    abs_avg = (sample_total > 0U) ? (uint32_t)(sum_abs / (uint64_t)sample_total) : 0U;
    talk_live_unlock();

    if (dropped > 0U) {
        ESP_LOGW(TAG,
                 "live ring overflow accepted=%" PRIu32 " dropped=%" PRIu32 " dropped_total=%" PRIu32,
                 accepted,
                 dropped,
                 dropped_total);
    }
    if (emit_diag) {
        uint32_t audio_ms = (uint32_t)(((uint64_t)diag_samples * 1000ULL) / TALK_LIVE_SAMPLE_RATE_HZ);
        ESP_LOGD(TAG,
                 "live rx diag chunks=%" PRIu32 " samples=%" PRIu32 " audio_ms=%" PRIu32
                 " frame[min=%d max=%d abs_avg=%" PRIu32 "]",
                 diag_chunks,
                 diag_samples,
                 audio_ms,
                 (int)min_v,
                 (int)max_v,
                 abs_avg);
    }
    return ESP_OK;
}
#endif
