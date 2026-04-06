#include "audio_task.h"

#include <stdbool.h>
#include "sdkconfig.h"
#include "app_tasking.h"
#include "audio_output_i2s.h"
#include "audio_worker.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;
static TaskHandle_t s_audio_task_handle;
static volatile bool s_audio_task_running;
static volatile bool s_stop_requested;

#if CONFIG_ORB_AUDIO_REAL_MP3_ENABLE
#define AUDIO_TASK_STACK_MIN_MP3 16384
#endif
#ifndef CONFIG_ORB_AUDIO_STOP_TIMEOUT_MS
#define CONFIG_ORB_AUDIO_STOP_TIMEOUT_MS 300
#endif
#ifndef CONFIG_ORB_AUDIO_STOP_SEND_RETRY_COUNT
#define CONFIG_ORB_AUDIO_STOP_SEND_RETRY_COUNT 8
#endif
#ifndef CONFIG_ORB_AUDIO_STOP_SEND_RETRY_DELAY_MS
#define CONFIG_ORB_AUDIO_STOP_SEND_RETRY_DELAY_MS 10
#endif
#ifndef CONFIG_ORB_AUDIO_STOP_SEND_TIMEOUT_MS
#define CONFIG_ORB_AUDIO_STOP_SEND_TIMEOUT_MS 10
#endif

static void audio_task_wake(void)
{
    TaskHandle_t handle = s_audio_task_handle;
    if (handle != NULL) {
#if defined(INCLUDE_xTaskAbortDelay) && (INCLUDE_xTaskAbortDelay == 1)
        (void)xTaskAbortDelay(handle);
#else
        xTaskNotifyGive(handle);
#endif
    }
}

static void audio_task_entry(void *arg)
{
    (void)arg;
    QueueHandle_t queue = app_tasking_get_audio_cmd_queue();
    if (queue == NULL) {
        ESP_LOGE(TAG, "audio_cmd_queue is not initialized");
        s_audio_task_running = false;
        s_audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "audio_task started");
    audio_worker_init();

    while (!s_stop_requested) {
        audio_command_t cmd = { 0 };
        uint32_t recv_timeout_ms = audio_worker_is_playing() ? 10U : (uint32_t)CONFIG_ORB_QUEUE_RECV_TIMEOUT_MS;
        if (recv_timeout_ms > 100U) {
            recv_timeout_ms = 100U;
        }
        BaseType_t got = xQueueReceive(queue, &cmd, pdMS_TO_TICKS(recv_timeout_ms));
        if (got == pdTRUE) {
            audio_worker_handle_command(&cmd);
        }
        audio_worker_poll();
    }

    /* Graceful self-owned shutdown path: stop playback and release hardware. */
    audio_command_t stop_cmd = { .id = AUDIO_CMD_STOP };
    audio_worker_handle_command(&stop_cmd);
    (void)audio_output_i2s_stop();

    s_audio_task_handle = NULL;
    s_audio_task_running = false;
    s_stop_requested = false;
    ESP_LOGI(TAG, "audio_task stopped");
    vTaskDelete(NULL);
}

esp_err_t audio_task_start(void)
{
    if (s_audio_task_handle != NULL) {
        return ESP_OK;
    }
    s_stop_requested = false;

    uint32_t stack_size = (uint32_t)CONFIG_ORB_AUDIO_TASK_STACK_SIZE;
#if CONFIG_ORB_AUDIO_REAL_MP3_ENABLE
    if (stack_size < AUDIO_TASK_STACK_MIN_MP3) {
        ESP_LOGW(TAG,
                 "audio stack %lu is too small for MP3; using %d",
                 (unsigned long)stack_size,
                 AUDIO_TASK_STACK_MIN_MP3);
        stack_size = AUDIO_TASK_STACK_MIN_MP3;
    }
#endif

    BaseType_t ok = xTaskCreate(audio_task_entry,
                                "audio_task",
                                stack_size,
                                NULL,
                                CONFIG_ORB_AUDIO_TASK_PRIORITY,
                                &s_audio_task_handle);
    if (ok != pdPASS) {
        s_audio_task_handle = NULL;
        s_audio_task_running = false;
        return ESP_FAIL;
    }
    s_audio_task_running = true;

    ESP_LOGI(TAG,
             "task created prio=%d stack=%lu",
             CONFIG_ORB_AUDIO_TASK_PRIORITY,
             (unsigned long)stack_size);
    return ESP_OK;
}

esp_err_t audio_task_stop(void)
{
    if (s_audio_task_handle == NULL) {
        return ESP_OK;
    }

    s_stop_requested = true;

    /* Wake task promptly and ask worker to stop active playback. */
    QueueHandle_t queue = app_tasking_get_audio_cmd_queue();
    bool stop_enqueued = false;
    if (queue != NULL) {
        audio_command_t stop_cmd = { .id = AUDIO_CMD_STOP };
        const TickType_t send_timeout_ticks = pdMS_TO_TICKS(CONFIG_ORB_AUDIO_STOP_SEND_TIMEOUT_MS);
        for (uint32_t i = 0; i < (uint32_t)CONFIG_ORB_AUDIO_STOP_SEND_RETRY_COUNT; ++i) {
            if (xQueueSend(queue, &stop_cmd, send_timeout_ticks) == pdTRUE) {
                stop_enqueued = true;
                break;
            }
            audio_task_wake();
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ORB_AUDIO_STOP_SEND_RETRY_DELAY_MS));
        }
    }
    if (!stop_enqueued) {
        ESP_LOGW(TAG, "audio_task stop command was not enqueued");
    }
    audio_task_wake();

    TickType_t wait_deadline = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)CONFIG_ORB_AUDIO_STOP_TIMEOUT_MS + 200U);
    while (s_audio_task_running) {
        if ((int32_t)(xTaskGetTickCount() - wait_deadline) >= 0) {
            ESP_LOGW(TAG, "audio_task stop timeout");
            return ESP_ERR_TIMEOUT;
        }
        audio_task_wake();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}
