#include "mic_task_tts_pipeline.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_tasking.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "mic_task_events.h"
#include "mic_ws_client.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_MIC_TASK_PRIORITY
#define CONFIG_ORB_MIC_TASK_PRIORITY 8
#endif
#ifndef CONFIG_ORB_MIC_TASK_CORE
#define CONFIG_ORB_MIC_TASK_CORE 0
#endif
#ifndef CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ
#define CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ 44100
#endif

#define MIC_TTS_PCM_QUEUE_TIMEOUT_MS 20U
#define MIC_TTS_PCM_QUEUE_RETRIES 4U
#define MIC_TTS_STREAM_STARTED_MARKER UINT16_MAX
#define MIC_TTS_RING_SECONDS 20U
#define MIC_TTS_RING_PUSH_WAIT_MS 2U
#define MIC_TTS_RING_PUSH_MAX_WAIT_MS 1200U
#define MIC_TTS_FEEDER_STACK_SIZE 8192U
#define MIC_TTS_FEEDER_PRIORITY (CONFIG_ORB_MIC_TASK_PRIORITY - 1)

static const char *TAG = LOG_TAG_MIC;

typedef struct {
    int16_t *samples;
    uint32_t capacity_samples;
    uint32_t write_pos;
    uint32_t read_pos;
    uint32_t count;
    uint32_t dropped_samples;
    bool stop_requested;
    TaskHandle_t feeder_task;
    bool feeder_stack_psram;
    portMUX_TYPE lock;
} tts_pcm_ring_t;

typedef struct {
    uint32_t dropped_chunks;
    uint32_t underflow_events;
    uint32_t underflow_loops;
    uint32_t max_ring_samples;
    TickType_t started_tick;
    bool first_chunk_logged;
    bool first_chunk_event_posted;
    TickType_t first_chunk_tick;
    TickType_t last_chunk_tick;
    TickType_t diag_last_log_tick;
    uint32_t chunk_count;
    uint32_t total_samples;
    uint32_t capture_id;
    tts_pcm_ring_t *ring;
} tts_stream_stats_t;

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static bool time_reached(TickType_t now, TickType_t deadline)
{
    return ((int32_t)(now - deadline) >= 0);
}

static bool push_pcm_stream_chunk(const int16_t *samples, uint16_t sample_count, uint32_t timeout_ms)
{
    if (samples == NULL || sample_count == 0U || sample_count > AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES) {
        return false;
    }

    audio_command_t cmd = { 0 };
    cmd.id = AUDIO_CMD_PCM_STREAM_CHUNK;
    cmd.payload.pcm_stream_chunk.sample_count = sample_count;
    memcpy(cmd.payload.pcm_stream_chunk.samples, samples, (size_t)sample_count * sizeof(int16_t));
    return (app_tasking_send_audio_command(&cmd, timeout_ms) == ESP_OK);
}

static void stop_pcm_stream_best_effort(void)
{
    audio_command_t stop_cmd = { .id = AUDIO_CMD_PCM_STREAM_STOP };
    const uint32_t timeout_ms = 20U;
    const uint32_t retries = 3U;
    for (uint32_t i = 0U; i <= retries; ++i) {
        esp_err_t err = app_tasking_send_audio_command(&stop_cmd, timeout_ms);
        if (err == ESP_OK) {
            return;
        }
        if (i < retries) {
            vTaskDelay(ms_to_ticks_min1(2U));
        }
    }
}

static bool tts_ring_init(tts_pcm_ring_t *ring, uint32_t sample_rate_hz)
{
    if (ring == NULL) {
        return false;
    }
    memset(ring, 0, sizeof(*ring));
    ring->capacity_samples = sample_rate_hz * MIC_TTS_RING_SECONDS;
    if (ring->capacity_samples < AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES * 8U) {
        ring->capacity_samples = AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES * 8U;
    }
    ring->samples = (int16_t *)heap_caps_malloc((size_t)ring->capacity_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring->samples == NULL) {
        ring->samples = (int16_t *)heap_caps_malloc((size_t)ring->capacity_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
        if (ring->samples == NULL) {
            return false;
        }
    }
    portMUX_INITIALIZE(&ring->lock);
    return true;
}

static void tts_ring_deinit(tts_pcm_ring_t *ring)
{
    if (ring == NULL) {
        return;
    }
    if (ring->samples != NULL) {
        heap_caps_free(ring->samples);
    }
    memset(ring, 0, sizeof(*ring));
}

static uint32_t tts_ring_push(tts_pcm_ring_t *ring, const int16_t *samples, uint32_t sample_count)
{
    if (ring == NULL || ring->samples == NULL || samples == NULL || sample_count == 0U) {
        return 0U;
    }
    uint32_t accepted = 0U;
    portENTER_CRITICAL(&ring->lock);
    for (uint32_t i = 0U; i < sample_count; ++i) {
        if (ring->count >= ring->capacity_samples) {
            ring->dropped_samples += (sample_count - i);
            break;
        }
        ring->samples[ring->write_pos] = samples[i];
        ring->write_pos++;
        if (ring->write_pos >= ring->capacity_samples) {
            ring->write_pos = 0U;
        }
        ring->count++;
        accepted++;
    }
    portEXIT_CRITICAL(&ring->lock);
    return accepted;
}

static uint16_t tts_ring_pop(tts_pcm_ring_t *ring, int16_t *out, uint16_t max_samples)
{
    if (ring == NULL || ring->samples == NULL || out == NULL || max_samples == 0U) {
        return 0U;
    }
    uint16_t copied = 0U;
    portENTER_CRITICAL(&ring->lock);
    uint32_t to_copy = ring->count;
    if (to_copy > max_samples) {
        to_copy = max_samples;
    }
    while (copied < to_copy) {
        out[copied++] = ring->samples[ring->read_pos];
        ring->read_pos++;
        if (ring->read_pos >= ring->capacity_samples) {
            ring->read_pos = 0U;
        }
        ring->count--;
    }
    portEXIT_CRITICAL(&ring->lock);
    return copied;
}

static uint32_t tts_ring_count(tts_pcm_ring_t *ring)
{
    if (ring == NULL) {
        return 0U;
    }
    uint32_t count = 0U;
    portENTER_CRITICAL(&ring->lock);
    count = ring->count;
    portEXIT_CRITICAL(&ring->lock);
    return count;
}

static void tts_ring_request_stop(tts_pcm_ring_t *ring)
{
    if (ring == NULL) {
        return;
    }
    portENTER_CRITICAL(&ring->lock);
    ring->stop_requested = true;
    portEXIT_CRITICAL(&ring->lock);
}

static bool tts_ring_stop_requested(tts_pcm_ring_t *ring)
{
    if (ring == NULL) {
        return true;
    }
    bool stop = false;
    portENTER_CRITICAL(&ring->lock);
    stop = ring->stop_requested;
    portEXIT_CRITICAL(&ring->lock);
    return stop;
}

static void tts_feeder_task(void *arg)
{
    tts_stream_stats_t *stats = (tts_stream_stats_t *)arg;
    if (stats == NULL || stats->ring == NULL) {
        vTaskDelete(NULL);
        return;
    }
    tts_pcm_ring_t *ring = stats->ring;
    int16_t *chunk = (int16_t *)heap_caps_malloc((size_t)AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES * sizeof(int16_t),
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        stats->dropped_chunks++;
        bool stack_psram = ring->feeder_stack_psram;
        ring->feeder_task = NULL;
        if (stack_psram) {
            vTaskDeleteWithCaps(NULL);
        } else {
            vTaskDelete(NULL);
        }
        return;
    }
    bool in_underflow = false;
    for (;;) {
        uint16_t n = tts_ring_pop(ring, chunk, AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES);
        if (n > 0U) {
            in_underflow = false;
            bool sent = push_pcm_stream_chunk(chunk, n, MIC_TTS_PCM_QUEUE_TIMEOUT_MS);
            for (uint32_t retry = 0U; !sent && retry < MIC_TTS_PCM_QUEUE_RETRIES; ++retry) {
                vTaskDelay(ms_to_ticks_min1(1U));
                sent = push_pcm_stream_chunk(chunk, n, MIC_TTS_PCM_QUEUE_TIMEOUT_MS);
            }
            if (!sent) {
                stats->dropped_chunks++;
            }
            continue;
        }
        bool stop_requested = tts_ring_stop_requested(ring);
        uint32_t ring_count = tts_ring_count(ring);
        if (!stop_requested && ring_count == 0U) {
            stats->underflow_loops++;
            if (!in_underflow) {
                in_underflow = true;
                stats->underflow_events++;
            }
        }
        if (stop_requested && ring_count == 0U) {
            break;
        }
        vTaskDelay(ms_to_ticks_min1(1U));
    }
    heap_caps_free(chunk);
    bool stack_psram = ring->feeder_stack_psram;
    ring->feeder_task = NULL;
    if (stack_psram) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

static void pcm_chunk_stats(const int16_t *samples,
                            uint16_t sample_count,
                            int16_t *out_min,
                            int16_t *out_max,
                            uint32_t *out_abs_avg,
                            uint16_t *out_clip_permille)
{
    if (samples == NULL || sample_count == 0U || out_min == NULL || out_max == NULL ||
        out_abs_avg == NULL || out_clip_permille == NULL) {
        return;
    }

    int16_t min_v = 32767;
    int16_t max_v = -32768;
    uint32_t clip = 0U;
    uint64_t sum_abs = 0U;
    for (uint16_t i = 0; i < sample_count; ++i) {
        int16_t v = samples[i];
        if (v < min_v) {
            min_v = v;
        }
        if (v > max_v) {
            max_v = v;
        }
        if (v >= 32760 || v <= -32760) {
            clip++;
        }
        int32_t iv = (int32_t)v;
        sum_abs += (uint64_t)((iv < 0) ? -iv : iv);
    }

    *out_min = min_v;
    *out_max = max_v;
    *out_abs_avg = (uint32_t)(sum_abs / sample_count);
    *out_clip_permille = (uint16_t)((clip * 1000U) / sample_count);
}

static esp_err_t tts_stream_chunk_cb(const int16_t *samples, uint16_t sample_count, void *user_ctx)
{
    tts_stream_stats_t *stats = (tts_stream_stats_t *)user_ctx;
    if (stats == NULL || stats->ring == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t now = xTaskGetTickCount();
    if (!stats->first_chunk_logged) {
        stats->first_chunk_logged = true;
        stats->first_chunk_tick = now;
        stats->diag_last_log_tick = now;
        uint32_t latency_ms = (uint32_t)((now - stats->started_tick) * portTICK_PERIOD_MS);
        ESP_LOGW(TAG, "tts stream first pcm chunk latency=%" PRIu32 "ms samples=%u", latency_ms, (unsigned)sample_count);
        if (!stats->first_chunk_event_posted) {
            stats->first_chunk_event_posted = true;
            mic_task_events_post_tts_stream_started(stats->capture_id, MIC_TTS_STREAM_STARTED_MARKER);
        }
    }

    stats->last_chunk_tick = now;
    stats->chunk_count++;
    stats->total_samples += sample_count;
    TickType_t log_gap = ms_to_ticks_min1(1000U);
    if ((now - stats->diag_last_log_tick) >= log_gap) {
        stats->diag_last_log_tick = now;
        uint32_t rx_ms = (uint32_t)((now - stats->first_chunk_tick) * portTICK_PERIOD_MS);
        uint32_t audio_ms = (uint32_t)(((uint64_t)stats->total_samples * 1000ULL) /
                                       (uint64_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ);
        int16_t min_v = 0;
        int16_t max_v = 0;
        uint32_t abs_avg = 0U;
        uint16_t clip_pm = 0U;
        pcm_chunk_stats(samples, sample_count, &min_v, &max_v, &abs_avg, &clip_pm);
        ESP_LOGD(TAG,
                 "tts stream diag chunks=%" PRIu32 " samples=%" PRIu32
                 " rx_ms=%" PRIu32 " audio_ms=%" PRIu32
                 " last[min=%d max=%d abs_avg=%" PRIu32 " clip=%u/1000]",
                 stats->chunk_count,
                 stats->total_samples,
                 rx_ms,
                 audio_ms,
                 (int)min_v,
                 (int)max_v,
                 abs_avg,
                 (unsigned)clip_pm);
    }

    const int16_t *cursor = samples;
    uint32_t remaining = sample_count;
    TickType_t deadline = now + ms_to_ticks_min1(MIC_TTS_RING_PUSH_MAX_WAIT_MS);
    while (remaining > 0U) {
        uint32_t accepted = tts_ring_push(stats->ring, cursor, remaining);
        if (accepted > 0U) {
            cursor += accepted;
            remaining -= accepted;
            continue;
        }
        if (time_reached(xTaskGetTickCount(), deadline)) {
            break;
        }
        vTaskDelay(ms_to_ticks_min1(MIC_TTS_RING_PUSH_WAIT_MS));
    }

    if (remaining > 0U) {
        stats->dropped_chunks++;
        return ESP_ERR_TIMEOUT;
    }
    uint32_t ring_fill = tts_ring_count(stats->ring);
    if (ring_fill > stats->max_ring_samples) {
        stats->max_ring_samples = ring_fill;
    }
    return ESP_OK;
}

void mic_task_tts_pipeline_play(uint32_t capture_id, const char *text, uint32_t stream_timeout_ms)
{
    if (text == NULL || text[0] == '\0') {
        stop_pcm_stream_best_effort();
        return;
    }

    TickType_t tts_start_tick = xTaskGetTickCount();
    tts_pcm_ring_t ring = { 0 };
    if (!tts_ring_init(&ring, (uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ)) {
        ESP_LOGW(TAG, "tts ring alloc failed");
        stop_pcm_stream_best_effort();
        mic_task_events_post_tts_error(ESP_ERR_NO_MEM);
        return;
    }
    tts_stream_stats_t stats = {
        .dropped_chunks = 0U,
        .underflow_events = 0U,
        .underflow_loops = 0U,
        .max_ring_samples = 0U,
        .started_tick = tts_start_tick,
        .first_chunk_logged = false,
        .first_chunk_event_posted = false,
        .first_chunk_tick = 0U,
        .last_chunk_tick = 0U,
        .diag_last_log_tick = 0U,
        .chunk_count = 0U,
        .total_samples = 0U,
        .capture_id = capture_id,
        .ring = &ring,
    };
    BaseType_t feeder_ok = pdFAIL;
    ring.feeder_stack_psram = false;
#if CONFIG_SPIRAM_USE_MALLOC
#if CONFIG_FREERTOS_UNICORE
    feeder_ok = xTaskCreateWithCaps(tts_feeder_task,
                                    "mic_tts_feed",
                                    MIC_TTS_FEEDER_STACK_SIZE,
                                    &stats,
                                    (MIC_TTS_FEEDER_PRIORITY > 0) ? MIC_TTS_FEEDER_PRIORITY : 1U,
                                    &ring.feeder_task,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    feeder_ok = xTaskCreatePinnedToCoreWithCaps(tts_feeder_task,
                                                "mic_tts_feed",
                                                MIC_TTS_FEEDER_STACK_SIZE,
                                                &stats,
                                                (MIC_TTS_FEEDER_PRIORITY > 0) ? MIC_TTS_FEEDER_PRIORITY : 1U,
                                                &ring.feeder_task,
                                                CONFIG_ORB_MIC_TASK_CORE,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (feeder_ok == pdPASS && ring.feeder_task != NULL) {
        ring.feeder_stack_psram = true;
    } else {
        ESP_LOGW(TAG, "mic_tts_feed PSRAM stack alloc failed, fallback to internal stack");
        ring.feeder_task = NULL;
    }
#endif
    if (feeder_ok != pdPASS || ring.feeder_task == NULL) {
#if CONFIG_FREERTOS_UNICORE
        feeder_ok = xTaskCreate(tts_feeder_task,
                                "mic_tts_feed",
                                MIC_TTS_FEEDER_STACK_SIZE,
                                &stats,
                                (MIC_TTS_FEEDER_PRIORITY > 0) ? MIC_TTS_FEEDER_PRIORITY : 1U,
                                &ring.feeder_task);
#else
        feeder_ok = xTaskCreatePinnedToCore(tts_feeder_task,
                                            "mic_tts_feed",
                                            MIC_TTS_FEEDER_STACK_SIZE,
                                            &stats,
                                            (MIC_TTS_FEEDER_PRIORITY > 0) ? MIC_TTS_FEEDER_PRIORITY : 1U,
                                            &ring.feeder_task,
                                            CONFIG_ORB_MIC_TASK_CORE);
#endif
    }
    if (feeder_ok != pdPASS || ring.feeder_task == NULL) {
        tts_ring_deinit(&ring);
        ESP_LOGW(TAG, "tts feeder task create failed");
        stop_pcm_stream_best_effort();
        mic_task_events_post_tts_error(ESP_ERR_NO_MEM);
        return;
    }
    esp_err_t err = mic_ws_client_tts_play(text,
                                           CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ,
                                           stream_timeout_ms,
                                           tts_stream_chunk_cb,
                                           &stats);
    tts_ring_request_stop(&ring);

    uint32_t pending_samples = tts_ring_count(&ring);
    uint32_t pending_audio_ms = (uint32_t)(((uint64_t)pending_samples * 1000ULL) /
                                           (uint64_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ);
    uint32_t drain_wait_ms = pending_audio_ms + 3000U;
    if (drain_wait_ms < 2000U) {
        drain_wait_ms = 2000U;
    }
    if (drain_wait_ms > 60000U) {
        drain_wait_ms = 60000U;
    }

    TickType_t wait_deadline = xTaskGetTickCount() + ms_to_ticks_min1(drain_wait_ms);
    while (ring.feeder_task != NULL && !time_reached(xTaskGetTickCount(), wait_deadline)) {
        vTaskDelay(ms_to_ticks_min1(10U));
    }
    if (ring.feeder_task != NULL) {
        TaskHandle_t handle = ring.feeder_task;
        bool stack_psram = ring.feeder_stack_psram;
        ring.feeder_task = NULL;
        if (stack_psram) {
            vTaskDeleteWithCaps(handle);
        } else {
            vTaskDelete(handle);
        }
        ESP_LOGW(TAG,
                 "tts feeder forced stop (pending=%" PRIu32 " samples, pending_ms=%" PRIu32
                 ", wait_ms=%" PRIu32 ")",
                 pending_samples,
                 pending_audio_ms,
                 drain_wait_ms);
    }
    tts_ring_deinit(&ring);
    stop_pcm_stream_best_effort();

    uint32_t total_ms = (uint32_t)((xTaskGetTickCount() - tts_start_tick) * portTICK_PERIOD_MS);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tts test failed: %s total=%" PRIu32 "ms first_chunk=%u",
                 esp_err_to_name(err),
                 total_ms,
                 stats.first_chunk_logged ? 1U : 0U);
        mic_task_events_post_tts_error(err);
    } else {
        uint32_t rx_span_ms = 0U;
        if (stats.first_chunk_logged && stats.last_chunk_tick >= stats.first_chunk_tick) {
            rx_span_ms = (uint32_t)((stats.last_chunk_tick - stats.first_chunk_tick) * portTICK_PERIOD_MS);
        }
        uint32_t audio_ms_equiv = (uint32_t)(((uint64_t)stats.total_samples * 1000ULL) /
                                             (uint64_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ);
        ESP_LOGI(TAG,
                 "tts test complete total=%" PRIu32 "ms dropped_chunks=%" PRIu32
                 " underflow_events=%" PRIu32 " underflow_loops=%" PRIu32
                 " max_ring=%" PRIu32 " chunks=%" PRIu32 " samples=%" PRIu32
                 " rx_span_ms=%" PRIu32 " audio_ms=%" PRIu32,
                 total_ms,
                 stats.dropped_chunks,
                 stats.underflow_events,
                 stats.underflow_loops,
                 stats.max_ring_samples,
                 stats.chunk_count,
                 stats.total_samples,
                 rx_span_ms,
                 audio_ms_equiv);
        mic_task_events_post_tts_done(stats.dropped_chunks);
    }
}
