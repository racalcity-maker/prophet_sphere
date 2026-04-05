#include "rest_api_talk_internal.h"

#if ORB_TALK_WS_ENABLED
#include <inttypes.h>
#include <string.h>
#endif
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_tags.h"

#if ORB_TALK_WS_ENABLED
static const char *TAG = LOG_TAG_REST;
#endif

#if ORB_TALK_WS_ENABLED
typedef struct {
    bool has_last_sample;
    int16_t last_sample;
    int32_t dc_prev_x;
    int32_t dc_prev_y;
} talk_live_postfx_state_t;

typedef struct {
    SemaphoreHandle_t lock;
    esp_timer_handle_t watchdog;
    bool websocket_open;
    bool pcm_stream_active;
    int sockfd;
    int64_t last_chunk_ms;
    int16_t *ring_samples;
    uint32_t ring_capacity_samples;
    uint32_t ring_write_pos;
    uint32_t ring_read_pos;
    uint32_t ring_count;
    uint32_t ring_dropped_samples;
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
    .ring_samples = NULL,
    .ring_capacity_samples = 0U,
    .ring_write_pos = 0U,
    .ring_read_pos = 0U,
    .ring_count = 0U,
    .ring_dropped_samples = 0U,
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

static inline int16_t talk_live_clamp_i16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

static void talk_live_postfx_reset_locked(void)
{
    s_live.postfx.has_last_sample = false;
    s_live.postfx.last_sample = 0;
    s_live.postfx.dc_prev_x = 0;
    s_live.postfx.dc_prev_y = 0;
}

static int16_t talk_live_postfx_soft_limit(int32_t x)
{
    int32_t ax = (x < 0) ? -x : x;
    if (ax <= TALK_LIVE_POSTFX_LIMITER_THRESHOLD) {
        return talk_live_clamp_i16(x);
    }
    int32_t over = ax - TALK_LIVE_POSTFX_LIMITER_THRESHOLD;
    int32_t comp = TALK_LIVE_POSTFX_LIMITER_THRESHOLD;
    comp += (int32_t)(((int64_t)over * (int64_t)TALK_LIVE_POSTFX_LIMITER_KNEE) /
                      (int64_t)(TALK_LIVE_POSTFX_LIMITER_KNEE + over));
    if (comp > 32767) {
        comp = 32767;
    }
    return (x < 0) ? (int16_t)(-comp) : (int16_t)comp;
}

static void talk_live_postfx_apply_inplace_locked(int16_t *samples, uint32_t sample_count)
{
    if (samples == NULL || sample_count == 0U) {
        return;
    }

    if (s_live.postfx.has_last_sample) {
        int32_t first = (int32_t)samples[0];
        int32_t prev = (int32_t)s_live.postfx.last_sample;
        int32_t jump = first - prev;
        if (jump < 0) {
            jump = -jump;
        }
        if (jump >= TALK_LIVE_POSTFX_DECLICK_THRESHOLD) {
            uint32_t ramp_n = sample_count;
            if (ramp_n > TALK_LIVE_POSTFX_DECLICK_RAMP_SAMPLES) {
                ramp_n = TALK_LIVE_POSTFX_DECLICK_RAMP_SAMPLES;
            }
            for (uint32_t i = 0U; i < ramp_n; ++i) {
                int32_t target = (int32_t)samples[i];
                int32_t mixed = prev + (int32_t)(((int64_t)(target - prev) * (int64_t)(i + 1U)) / (int64_t)ramp_n);
                samples[i] = talk_live_clamp_i16(mixed);
            }
        }
    }

    int32_t prev_x = s_live.postfx.dc_prev_x;
    int32_t prev_y = s_live.postfx.dc_prev_y;
    for (uint32_t i = 0U; i < sample_count; ++i) {
        int32_t x = (int32_t)samples[i];
        int32_t y = (x - prev_x) +
                    (int32_t)(((int64_t)TALK_LIVE_POSTFX_DC_BETA_Q15 * (int64_t)prev_y) >> 15);
        prev_x = x;
        prev_y = y;
        samples[i] = talk_live_postfx_soft_limit(y);
    }

    s_live.postfx.dc_prev_x = prev_x;
    s_live.postfx.dc_prev_y = prev_y;
    s_live.postfx.last_sample = samples[sample_count - 1U];
    s_live.postfx.has_last_sample = true;
}

static void talk_live_ring_reset_locked(void)
{
    s_live.ring_write_pos = 0U;
    s_live.ring_read_pos = 0U;
    s_live.ring_count = 0U;
}

static uint32_t talk_live_ring_push_locked(const int16_t *samples, uint32_t sample_count)
{
    if (samples == NULL || sample_count == 0U || s_live.ring_samples == NULL || s_live.ring_capacity_samples == 0U) {
        return 0U;
    }

    uint32_t accepted = 0U;
    while (accepted < sample_count) {
        if (s_live.ring_count >= s_live.ring_capacity_samples) {
            s_live.ring_dropped_samples += (sample_count - accepted);
            break;
        }
        s_live.ring_samples[s_live.ring_write_pos] = samples[accepted];
        s_live.ring_write_pos++;
        if (s_live.ring_write_pos >= s_live.ring_capacity_samples) {
            s_live.ring_write_pos = 0U;
        }
        s_live.ring_count++;
        accepted++;
    }
    return accepted;
}

static uint16_t talk_live_ring_pop_locked(int16_t *out_samples, uint16_t max_samples)
{
    if (out_samples == NULL || max_samples == 0U || s_live.ring_samples == NULL || s_live.ring_capacity_samples == 0U) {
        return 0U;
    }
    uint32_t to_copy = s_live.ring_count;
    if (to_copy > max_samples) {
        to_copy = max_samples;
    }
    uint16_t copied = 0U;
    while (copied < to_copy) {
        out_samples[copied++] = s_live.ring_samples[s_live.ring_read_pos];
        s_live.ring_read_pos++;
        if (s_live.ring_read_pos >= s_live.ring_capacity_samples) {
            s_live.ring_read_pos = 0U;
        }
        s_live.ring_count--;
    }
    return copied;
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

        if (s_live.websocket_open && !s_live.pcm_stream_active && s_live.ring_count >= TALK_LIVE_PREBUFFER_SAMPLES) {
            s_live.pcm_stream_active = true;
            need_start = true;
            buffered_samples = s_live.ring_count;
        }

        if (s_live.pcm_stream_active) {
            chunk_samples = talk_live_ring_pop_locked(chunk, AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES);
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
                    talk_live_ring_reset_locked();
                    talk_live_postfx_reset_locked();
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
        talk_live_ring_reset_locked();
        talk_live_postfx_reset_locked();
        s_live.ring_dropped_samples = 0U;
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

    if (s_live.watchdog == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = talk_live_watchdog_cb,
            .arg = NULL,
            .name = "talk_live_wd",
            .dispatch_method = ESP_TIMER_TASK,
            .skip_unhandled_events = true,
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_live.watchdog), TAG, "live watchdog create failed");
        ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_live.watchdog, TALK_LIVE_WATCHDOG_PERIOD_MS * 1000ULL),
                            TAG,
                            "live watchdog start failed");
    }

    if (s_live.ring_samples == NULL) {
        s_live.ring_capacity_samples = TALK_LIVE_RING_CAPACITY_SAMPLES;
        s_live.ring_samples = (int16_t *)heap_caps_malloc((size_t)s_live.ring_capacity_samples * sizeof(int16_t),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_live.ring_samples == NULL) {
            s_live.ring_samples = (int16_t *)heap_caps_malloc((size_t)s_live.ring_capacity_samples * sizeof(int16_t),
                                                              MALLOC_CAP_8BIT);
        }
        if (s_live.ring_samples == NULL) {
            return ESP_ERR_NO_MEM;
        }
        talk_live_ring_reset_locked();
        talk_live_postfx_reset_locked();
        s_live.ring_dropped_samples = 0U;
    }

    if (s_live.feeder_task == NULL) {
        BaseType_t ok = pdFAIL;
        s_live.feeder_stack_psram = false;
#if CONFIG_SPIRAM_USE_MALLOC
#if CONFIG_FREERTOS_UNICORE
        ok = xTaskCreateWithCaps(talk_live_feeder_task,
                                 "talk_live_feed",
                                 TALK_LIVE_FEED_TASK_STACK,
                                 NULL,
                                 TALK_LIVE_FEED_TASK_PRIORITY,
                                 &s_live.feeder_task,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        ok = xTaskCreatePinnedToCoreWithCaps(talk_live_feeder_task,
                                             "talk_live_feed",
                                             TALK_LIVE_FEED_TASK_STACK,
                                             NULL,
                                             TALK_LIVE_FEED_TASK_PRIORITY,
                                             &s_live.feeder_task,
                                             tskNO_AFFINITY,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (ok == pdPASS && s_live.feeder_task != NULL) {
            s_live.feeder_stack_psram = true;
        } else {
            s_live.feeder_task = NULL;
        }
#endif
        if (ok != pdPASS || s_live.feeder_task == NULL) {
            s_live.feeder_stack_psram = false;
            ok = xTaskCreatePinnedToCore(talk_live_feeder_task,
                                         "talk_live_feed",
                                         TALK_LIVE_FEED_TASK_STACK,
                                         NULL,
                                         TALK_LIVE_FEED_TASK_PRIORITY,
                                         &s_live.feeder_task,
                                         tskNO_AFFINITY);
        }
        if (ok != pdPASS || s_live.feeder_task == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
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
        talk_live_ring_reset_locked();
        talk_live_postfx_reset_locked();
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
    s_live.ring_dropped_samples = 0U;
    s_live.rx_chunks = 0U;
    s_live.rx_samples = 0U;
    s_live.rx_diag_last_ms = 0;
    talk_live_ring_reset_locked();
    talk_live_postfx_reset_locked();
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
    talk_live_postfx_apply_inplace_locked(in_samples, sample_total);
    uint32_t accepted = talk_live_ring_push_locked(in_samples, sample_total);
    uint32_t dropped = sample_total - accepted;
    uint32_t dropped_total = s_live.ring_dropped_samples;
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
