#include "mic_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_tasking.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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
#define MIC_LOOPBACK_READ_TIMEOUT_MS 5U
#ifndef CONFIG_ORB_MIC_WS_ENABLE
#define CONFIG_ORB_MIC_WS_ENABLE 0
#endif
static TaskHandle_t s_mic_task_handle;
static QueueHandle_t s_command_queue;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_stop_requested;
static mic_capture_status_t s_status;

static void finish_capture(mic_capture_ctx_t *ctx, mic_loopback_ctx_t *loopback, bool post_done);
static void stop_loopback(mic_capture_ctx_t *capture, mic_loopback_ctx_t *loopback);

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

    int32_t sample_buf[CONFIG_ORB_MIC_READ_CHUNK_SAMPLES] = { 0 };

    while (!s_stop_requested) {
        mic_command_t cmd = { 0 };

        if (!capture.active && !loopback.active) {
            BaseType_t got = xQueueReceive(s_command_queue, &cmd, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_RECV_TIMEOUT_MS));
            if (got == pdTRUE) {
                mic_task_flow_process_command(&capture, &loopback, &cmd, &s_flow_ops, TAG);
            }
            continue;
        }

        if (xQueueReceive(s_command_queue, &cmd, 0) == pdTRUE) {
            mic_task_flow_process_command(&capture, &loopback, &cmd, &s_flow_ops, TAG);
            if (!capture.active && !loopback.active) {
                continue;
            }
        }

        size_t read_samples = 0U;
        uint32_t read_timeout_ms = loopback.active ? MIC_LOOPBACK_READ_TIMEOUT_MS : (uint32_t)CONFIG_ORB_MIC_READ_TIMEOUT_MS;
        esp_err_t err =
            mic_i2s_hal_read_i32(sample_buf, CONFIG_ORB_MIC_READ_CHUNK_SAMPLES, &read_samples, read_timeout_ms);
        if (err == ESP_ERR_TIMEOUT) {
            if (capture.active && time_reached(xTaskGetTickCount(), capture.deadline_tick)) {
                mic_task_capture_finalize_intent(&capture, TAG);
                finish_capture(&capture, &loopback, true);
            }
            mic_capture_loop_cooperate();
            continue;
        }
        if (err != ESP_OK) {
            uint32_t failed_id = capture.capture_id;
            if (capture.active) {
                finish_capture(&capture, &loopback, false);
            }
            if (loopback.active) {
                stop_loopback(&capture, &loopback);
            }
            mic_task_events_post_capture_error(failed_id, err);
            continue;
        }

        if (capture.active) {
            mic_task_capture_accumulate_metrics(&capture, sample_buf, read_samples);
            mic_task_capture_push_ws_chunk(&capture, sample_buf, read_samples, TAG);
            update_status(&capture, loopback.active);

            if (time_reached(xTaskGetTickCount(), capture.deadline_tick)) {
                mic_task_capture_finalize_intent(&capture, TAG);
                finish_capture(&capture, &loopback, true);
            }
        } else if (loopback.active) {
            stream_loopback_samples(&loopback, sample_buf, read_samples);
            update_status(&capture, loopback.active);
        }

        mic_capture_loop_cooperate();
    }

    if (capture.active) {
        finish_capture(&capture, &loopback, false);
    }
    if (loopback.active) {
        stop_loopback(&capture, &loopback);
    }
    (void)mic_i2s_hal_deinit();
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

    s_command_queue = command_queue;
    s_stop_requested = false;
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
                     CONFIG_ORB_MIC_TASK_STACK_SIZE,
                     NULL,
                     CONFIG_ORB_MIC_TASK_PRIORITY,
                     &s_mic_task_handle);
#else
    ok = xTaskCreatePinnedToCore(mic_task_entry,
                                 "mic_task",
                                 CONFIG_ORB_MIC_TASK_STACK_SIZE,
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
             CONFIG_ORB_MIC_TASK_STACK_SIZE,
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

    s_stop_requested = true;
    if (s_command_queue != NULL) {
        mic_command_t cmd1 = { .id = MIC_CMD_STOP_CAPTURE };
        mic_command_t cmd2 = { .id = MIC_CMD_LOOPBACK_STOP };
        (void)xQueueSend(s_command_queue, &cmd1, 0);
        (void)xQueueSend(s_command_queue, &cmd2, 0);
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    while (s_mic_task_handle != NULL && !time_reached(xTaskGetTickCount(), deadline)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_mic_task_handle != NULL) {
        TaskHandle_t handle = s_mic_task_handle;
        s_mic_task_handle = NULL;
        vTaskDelete(handle);
        (void)mic_i2s_hal_deinit();
    }

#if CONFIG_ORB_MIC_WS_ENABLE
    mic_ws_client_deinit();
#endif
    s_command_queue = NULL;
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
