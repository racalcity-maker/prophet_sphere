#include "mic_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_tasking.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "mic_i2s_hal.h"
#include "mic_task_capture_ws.h"
#include "mic_task_events.h"
#include "mic_task_flow.h"
#include "mic_task_loopback.h"
#include "mic_task_types.h"
#include "mic_ws_client.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MIC;

#ifndef CONFIG_ORB_MIC_READ_CHUNK_SAMPLES
#define CONFIG_ORB_MIC_READ_CHUNK_SAMPLES 256
#endif
#ifndef CONFIG_ORB_MIC_READ_TIMEOUT_MS
#define CONFIG_ORB_MIC_READ_TIMEOUT_MS 30
#endif
#ifndef CONFIG_ORB_MIC_ACTIVE_LOOP_DELAY_MS
#define CONFIG_ORB_MIC_ACTIVE_LOOP_DELAY_MS 1
#endif
#ifndef CONFIG_ORB_MIC_INPUT_SHIFT
#define CONFIG_ORB_MIC_INPUT_SHIFT 13
#endif
#ifndef CONFIG_ORB_MIC_TASK_STACK_SIZE
#define CONFIG_ORB_MIC_TASK_STACK_SIZE 6144
#endif
#ifndef CONFIG_ORB_MIC_TASK_PRIORITY
#define CONFIG_ORB_MIC_TASK_PRIORITY 8
#endif
#ifndef CONFIG_ORB_MIC_TASK_CORE
#define CONFIG_ORB_MIC_TASK_CORE 0
#endif

#define MIC_TASK_STACK_MIN_SAFE 8192
#define MIC_TASK_STACK_SIZE_EFFECTIVE \
    ((CONFIG_ORB_MIC_TASK_STACK_SIZE < MIC_TASK_STACK_MIN_SAFE) ? MIC_TASK_STACK_MIN_SAFE : CONFIG_ORB_MIC_TASK_STACK_SIZE)

#define MIC_LOOPBACK_READ_TIMEOUT_MS 5U
#ifndef CONFIG_ORB_MIC_STOP_TIMEOUT_MS
#define CONFIG_ORB_MIC_STOP_TIMEOUT_MS 1500
#endif
#ifndef CONFIG_ORB_MIC_WS_ENABLE
#define CONFIG_ORB_MIC_WS_ENABLE 0
#endif
static TaskHandle_t s_mic_task_handle;
static QueueHandle_t s_command_queue;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_stop_requested;
static EventGroupHandle_t s_lifecycle_events;
static mic_capture_status_t s_status;
static int32_t s_sample_buf[CONFIG_ORB_MIC_READ_CHUNK_SAMPLES];

typedef enum {
    MIC_TASK_LOOP_CONTINUE = 0,
    MIC_TASK_LOOP_BREAK,
} mic_task_loop_action_t;

static void finish_capture(mic_capture_ctx_t *ctx, mic_loopback_ctx_t *loopback, bool post_done);
static void stop_loopback(mic_capture_ctx_t *capture, mic_loopback_ctx_t *loopback);
static TickType_t ms_to_ticks_min1(uint32_t ms);
static bool time_reached(TickType_t now, TickType_t deadline);

#define MIC_TASK_LIFECYCLE_STOP_REQUESTED BIT0
#define MIC_TASK_LIFECYCLE_STOPPED BIT1

typedef enum {
    MIC_TASK_STOP_PHASE_REQUESTED = 0,
    MIC_TASK_STOP_PHASE_DRAINING,
    MIC_TASK_STOP_PHASE_WAITING,
    MIC_TASK_STOP_PHASE_COMPLETED,
    MIC_TASK_STOP_PHASE_TIMEOUT,
} mic_task_stop_phase_t;

static void mic_task_log_stop_phase(mic_task_stop_phase_t phase)
{
    switch (phase) {
    case MIC_TASK_STOP_PHASE_REQUESTED:
        ESP_LOGI(TAG, "mic_task stop: requested");
        break;
    case MIC_TASK_STOP_PHASE_DRAINING:
        ESP_LOGI(TAG, "mic_task stop: draining");
        break;
    case MIC_TASK_STOP_PHASE_WAITING:
        ESP_LOGI(TAG, "mic_task stop: waiting");
        break;
    case MIC_TASK_STOP_PHASE_COMPLETED:
        ESP_LOGI(TAG, "mic_task stop: completed");
        break;
    case MIC_TASK_STOP_PHASE_TIMEOUT:
        ESP_LOGW(TAG, "mic_task stop: timeout");
        break;
    default:
        break;
    }
}

static void mic_task_request_stop_signal(void)
{
    s_stop_requested = true;
    if (s_lifecycle_events != NULL) {
        (void)xEventGroupSetBits(s_lifecycle_events, MIC_TASK_LIFECYCLE_STOP_REQUESTED);
    }
}

static esp_err_t mic_task_wait_stopped(TickType_t stop_wait_ticks)
{
    if (stop_wait_ticks == 0) {
        stop_wait_ticks = 1;
    }

    if (s_lifecycle_events != NULL) {
        EventBits_t bits = xEventGroupWaitBits(s_lifecycle_events,
                                               MIC_TASK_LIFECYCLE_STOPPED,
                                               pdFALSE,
                                               pdFALSE,
                                               stop_wait_ticks);
        if ((bits & MIC_TASK_LIFECYCLE_STOPPED) == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        return ESP_OK;
    }

    TickType_t deadline = xTaskGetTickCount() + stop_wait_ticks;
    while (s_mic_task_handle != NULL && !time_reached(xTaskGetTickCount(), deadline)) {
        vTaskDelay(ms_to_ticks_min1(10U));
    }
    return (s_mic_task_handle == NULL) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static bool mic_task_is_stop_requested(void)
{
    if (s_stop_requested) {
        return true;
    }
    EventGroupHandle_t events = s_lifecycle_events;
    if (events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(events);
    return ((bits & MIC_TASK_LIFECYCLE_STOP_REQUESTED) != 0U);
}

static void mic_task_wake(void)
{
    TaskHandle_t handle = s_mic_task_handle;
    if (handle != NULL) {
#if defined(INCLUDE_xTaskAbortDelay) && (INCLUDE_xTaskAbortDelay == 1)
        (void)xTaskAbortDelay(handle);
#else
        xTaskNotifyGive(handle);
        if (s_command_queue != NULL) {
            mic_command_t wake = { .id = MIC_CMD_NONE };
            TickType_t wait_ticks = ms_to_ticks_min1(10U);
            for (uint32_t attempt = 0U; attempt < 3U; ++attempt) {
                if (xQueueSend(s_command_queue, &wake, wait_ticks) == pdTRUE) {
                    break;
                }
                vTaskDelay(ms_to_ticks_min1(2U));
            }
        }
#endif
    }
}

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static bool time_reached(TickType_t now, TickType_t deadline)
{
    return ((int32_t)(now - deadline) >= 0);
}

static void mic_capture_loop_cooperate(void)
{
#if CONFIG_ORB_MIC_ACTIVE_LOOP_DELAY_MS > 0
    vTaskDelay(ms_to_ticks_min1(CONFIG_ORB_MIC_ACTIVE_LOOP_DELAY_MS));
#else
    taskYIELD();
#endif
}

static void update_status(const mic_capture_ctx_t *capture, bool loopback_active)
{
    mic_capture_status_t snapshot = { 0 };
    if (capture != NULL) {
        snapshot.active = capture->active || loopback_active;
        snapshot.capture_id = capture->capture_id;
        snapshot.captured_samples = capture->sample_count;
        snapshot.level_peak = capture->peak;
        snapshot.intent_id = capture->intent_id;
        snapshot.intent_confidence_permille = capture->intent_confidence_permille;
        if (capture->sample_count > 0U) {
            uint32_t avg = (uint32_t)(capture->abs_sum / capture->sample_count);
            if (avg > UINT16_MAX) {
                avg = UINT16_MAX;
            }
            snapshot.level_avg = (uint16_t)avg;
        }
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status = snapshot;
    portEXIT_CRITICAL(&s_status_lock);
}

static void stream_loopback_samples(mic_loopback_ctx_t *loopback, const int32_t *samples, size_t sample_count)
{
    if (loopback == NULL || !loopback->active || samples == NULL || sample_count == 0U) {
        return;
    }
    mic_task_loopback_stream_samples(&loopback->phase_accum, samples, sample_count, TAG);
}

static void finish_capture(mic_capture_ctx_t *ctx, mic_loopback_ctx_t *loopback, bool post_done)
{
    if (ctx == NULL || loopback == NULL) {
        return;
    }

#if CONFIG_ORB_MIC_WS_ENABLE
    if (ctx->ws_streaming) {
        /* See mic_task_capture_finalize_intent(): avoid hard abort during active event-loop churn. */
        ctx->ws_streaming = false;
    }
#endif

    (void)mic_i2s_hal_stop();
    ctx->active = false;
    update_status(ctx, loopback->active);

    if (post_done) {
        mic_task_events_post_capture_done(ctx->capture_id,
                                          ctx->sample_count,
                                          ctx->abs_sum,
                                          ctx->peak,
                                          ctx->intent_id,
                                          ctx->intent_confidence_permille);
        if (ctx->ws_result_used) {
            mic_task_events_post_remote_plan_ready(ctx->capture_id,
                                                   ctx->sample_count,
                                                   ctx->abs_sum,
                                                   ctx->peak,
                                                   ctx->intent_id,
                                                   ctx->intent_confidence_permille);
        } else if (ctx->ws_last_error != ESP_OK) {
            mic_task_events_post_remote_plan_error(ctx->capture_id, ctx->ws_last_error);
        }
    }
}

static void stop_loopback(mic_capture_ctx_t *capture, mic_loopback_ctx_t *loopback)
{
    if (capture == NULL || loopback == NULL || !loopback->active) {
        return;
    }
    (void)mic_i2s_hal_stop();
    loopback->active = false;
    loopback->phase_accum = 0U;
    update_status(capture, loopback->active);
    ESP_LOGI(TAG, "loopback stop");
}

static const mic_task_flow_ops_t s_flow_ops = {
    .finish_capture = finish_capture,
    .stop_loopback = stop_loopback,
    .update_status = update_status,
};

static void process_idle_command_wait(mic_capture_ctx_t *capture, mic_loopback_ctx_t *loopback)
{
    if (capture == NULL || loopback == NULL) {
        return;
    }
    mic_command_t cmd = { 0 };
    BaseType_t got = xQueueReceive(s_command_queue, &cmd, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_RECV_TIMEOUT_MS));
    if (got == pdTRUE) {
        mic_task_flow_process_command(capture, loopback, &cmd, &s_flow_ops, TAG);
    }
}

static bool process_active_command_poll(mic_capture_ctx_t *capture, mic_loopback_ctx_t *loopback)
{
    if (capture == NULL || loopback == NULL) {
        return false;
    }
    mic_command_t cmd = { 0 };
    if (xQueueReceive(s_command_queue, &cmd, 0) != pdTRUE) {
        return false;
    }

    mic_task_flow_process_command(capture, loopback, &cmd, &s_flow_ops, TAG);
    return (!capture->active && !loopback->active);
}

static void process_active_capture_iteration(mic_capture_ctx_t *capture,
                                             mic_loopback_ctx_t *loopback,
                                             const int32_t *samples,
                                             size_t sample_count)
{
    if (capture == NULL || loopback == NULL || samples == NULL || sample_count == 0U) {
        return;
    }
    mic_task_capture_accumulate_metrics(capture, samples, sample_count);
    mic_task_capture_push_ws_chunk(capture, samples, sample_count, TAG);
    update_status(capture, loopback->active);

    if (time_reached(xTaskGetTickCount(), capture->deadline_tick)) {
        mic_task_capture_finalize_intent(capture, TAG);
        finish_capture(capture, loopback, true);
    }
}

static void process_loopback_iteration(mic_capture_ctx_t *capture,
                                       mic_loopback_ctx_t *loopback,
                                       const int32_t *samples,
                                       size_t sample_count)
{
    if (capture == NULL || loopback == NULL || samples == NULL || sample_count == 0U) {
        return;
    }
    stream_loopback_samples(loopback, samples, sample_count);
    update_status(capture, loopback->active);
}

static mic_task_loop_action_t process_stream_iteration(mic_capture_ctx_t *capture,
                                                       mic_loopback_ctx_t *loopback,
                                                       int32_t *sample_buf,
                                                       size_t sample_buf_count)
{
    if (capture == NULL || loopback == NULL || sample_buf == NULL || sample_buf_count == 0U) {
        return MIC_TASK_LOOP_CONTINUE;
    }

    size_t read_samples = 0U;
    uint32_t read_timeout_ms = loopback->active ? MIC_LOOPBACK_READ_TIMEOUT_MS : (uint32_t)CONFIG_ORB_MIC_READ_TIMEOUT_MS;
    esp_err_t err = mic_i2s_hal_read_i32(sample_buf, sample_buf_count, &read_samples, read_timeout_ms);

    if (err == ESP_ERR_TIMEOUT) {
        if (capture->active && time_reached(xTaskGetTickCount(), capture->deadline_tick)) {
            mic_task_capture_finalize_intent(capture, TAG);
            finish_capture(capture, loopback, true);
        }
        mic_capture_loop_cooperate();
        return MIC_TASK_LOOP_CONTINUE;
    }

    if (err != ESP_OK) {
        if (mic_task_is_stop_requested()) {
            return MIC_TASK_LOOP_BREAK;
        }
        uint32_t failed_id = capture->capture_id;
        if (capture->active) {
            finish_capture(capture, loopback, false);
        }
        if (loopback->active) {
            stop_loopback(capture, loopback);
        }
        mic_task_events_post_capture_error(failed_id, err);
        return MIC_TASK_LOOP_CONTINUE;
    }

    if (capture->active) {
        process_active_capture_iteration(capture, loopback, sample_buf, read_samples);
    } else if (loopback->active) {
        process_loopback_iteration(capture, loopback, sample_buf, read_samples);
    }

    mic_capture_loop_cooperate();
    return MIC_TASK_LOOP_CONTINUE;
}

static void mic_task_entry(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "mic_task started");
    ESP_LOGI(TAG,
             "capture loop cooperate delay=%dms read_chunk=%d read_timeout=%dms",
             CONFIG_ORB_MIC_ACTIVE_LOOP_DELAY_MS,
             CONFIG_ORB_MIC_READ_CHUNK_SAMPLES,
             CONFIG_ORB_MIC_READ_TIMEOUT_MS);

    mic_capture_ctx_t capture = { 0 };
    mic_loopback_ctx_t loopback = { 0 };
    update_status(&capture, loopback.active);

    while (!mic_task_is_stop_requested()) {
        if (!capture.active && !loopback.active) {
            process_idle_command_wait(&capture, &loopback);
            continue;
        }

        if (process_active_command_poll(&capture, &loopback)) {
            continue;
        }

        if (process_stream_iteration(&capture, &loopback, s_sample_buf, CONFIG_ORB_MIC_READ_CHUNK_SAMPLES) == MIC_TASK_LOOP_BREAK) {
            break;
        }
    }

    if (capture.active) {
        finish_capture(&capture, &loopback, false);
    }
    if (loopback.active) {
        stop_loopback(&capture, &loopback);
    }
    (void)mic_i2s_hal_deinit();
#if CONFIG_ORB_MIC_WS_ENABLE
    mic_ws_client_deinit();
#endif
    s_command_queue = NULL;
    if (s_lifecycle_events != NULL) {
        (void)xEventGroupSetBits(s_lifecycle_events, MIC_TASK_LIFECYCLE_STOPPED);
    }
    s_stop_requested = false;
    s_mic_task_handle = NULL;
    ESP_LOGI(TAG, "mic_task stopped");
    vTaskDelete(NULL);
}

esp_err_t mic_task_start(QueueHandle_t command_queue)
{
    if (command_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mic_task_handle != NULL) {
        return ESP_OK;
    }
    if (s_lifecycle_events == NULL) {
        s_lifecycle_events = xEventGroupCreate();
        if (s_lifecycle_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_command_queue = command_queue;
    s_stop_requested = false;
    (void)xEventGroupClearBits(s_lifecycle_events, MIC_TASK_LIFECYCLE_STOP_REQUESTED | MIC_TASK_LIFECYCLE_STOPPED);
    memset(&s_status, 0, sizeof(s_status));

    ESP_RETURN_ON_ERROR(mic_i2s_hal_init(), TAG, "mic i2s init failed");
#if CONFIG_ORB_MIC_WS_ENABLE
    {
        esp_err_t ws_err = mic_ws_client_init();
        if (ws_err != ESP_OK) {
            ESP_LOGW(TAG, "mic ws init failed (%s)", esp_err_to_name(ws_err));
        }
    }
#endif

    BaseType_t ok;
#if CONFIG_FREERTOS_UNICORE
    ok = xTaskCreate(mic_task_entry,
                     "mic_task",
                     MIC_TASK_STACK_SIZE_EFFECTIVE,
                     NULL,
                     CONFIG_ORB_MIC_TASK_PRIORITY,
                     &s_mic_task_handle);
#else
    ok = xTaskCreatePinnedToCore(mic_task_entry,
                                 "mic_task",
                                 MIC_TASK_STACK_SIZE_EFFECTIVE,
                                 NULL,
                                 CONFIG_ORB_MIC_TASK_PRIORITY,
                                 &s_mic_task_handle,
                                 CONFIG_ORB_MIC_TASK_CORE);
#endif
    if (ok != pdPASS) {
        s_mic_task_handle = NULL;
        s_command_queue = NULL;
        (void)mic_i2s_hal_deinit();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "task created prio=%d stack=%d core=%d",
             CONFIG_ORB_MIC_TASK_PRIORITY,
             MIC_TASK_STACK_SIZE_EFFECTIVE,
             CONFIG_ORB_MIC_TASK_CORE);
    return ESP_OK;
}

esp_err_t mic_task_stop(void)
{
    if (s_mic_task_handle == NULL) {
#if CONFIG_ORB_MIC_WS_ENABLE
        mic_ws_client_deinit();
#endif
        return ESP_OK;
    }

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (self == s_mic_task_handle) {
        mic_task_request_stop_signal();
        return ESP_OK;
    }

    /* Unified stop contract for mic task:
     * requested -> draining -> waiting -> completed | timeout. */
    mic_task_log_stop_phase(MIC_TASK_STOP_PHASE_REQUESTED);
    mic_task_request_stop_signal();
    if (s_lifecycle_events != NULL) {
        (void)xEventGroupClearBits(s_lifecycle_events, MIC_TASK_LIFECYCLE_STOPPED);
    }

    mic_task_log_stop_phase(MIC_TASK_STOP_PHASE_DRAINING);
#if CONFIG_ORB_MIC_WS_ENABLE
    mic_ws_client_abort();
#endif
    (void)mic_i2s_hal_stop();
    mic_task_wake();

    mic_task_log_stop_phase(MIC_TASK_STOP_PHASE_WAITING);
    TickType_t stop_wait_ticks = pdMS_TO_TICKS(CONFIG_ORB_MIC_STOP_TIMEOUT_MS);
    esp_err_t wait_err = mic_task_wait_stopped(stop_wait_ticks);
    if (wait_err != ESP_OK) {
        mic_task_log_stop_phase(MIC_TASK_STOP_PHASE_TIMEOUT);
        return wait_err;
    }

    mic_task_log_stop_phase(MIC_TASK_STOP_PHASE_COMPLETED);
    return ESP_OK;
}

esp_err_t mic_task_get_status(mic_capture_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_status_lock);
    *out_status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}
