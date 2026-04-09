#include "audio_task.h"

#include <stdbool.h>
#include "sdkconfig.h"
#include "app_tasking.h"
#include "audio_output_i2s.h"
#include "audio_worker.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;
static TaskHandle_t s_audio_task_handle;
static volatile bool s_audio_task_running;
static volatile bool s_stop_requested;
static EventGroupHandle_t s_lifecycle_events;

#if CONFIG_ORB_AUDIO_REAL_MP3_ENABLE
#define AUDIO_TASK_STACK_MIN_MP3 16384
#endif
#ifndef CONFIG_ORB_AUDIO_STOP_TIMEOUT_MS
#define CONFIG_ORB_AUDIO_STOP_TIMEOUT_MS 300
#endif

#define AUDIO_TASK_LIFECYCLE_STOP_REQUESTED BIT0
#define AUDIO_TASK_LIFECYCLE_STOPPED BIT1

typedef enum {
    AUDIO_TASK_STOP_PHASE_REQUESTED = 0,
    AUDIO_TASK_STOP_PHASE_WAITING,
    AUDIO_TASK_STOP_PHASE_COMPLETED,
    AUDIO_TASK_STOP_PHASE_TIMEOUT,
} audio_task_stop_phase_t;

static void audio_task_log_stop_phase(audio_task_stop_phase_t phase)
{
    switch (phase) {
    case AUDIO_TASK_STOP_PHASE_REQUESTED:
        ESP_LOGI(TAG, "audio_task stop: requested");
        break;
    case AUDIO_TASK_STOP_PHASE_WAITING:
        ESP_LOGI(TAG, "audio_task stop: waiting");
        break;
    case AUDIO_TASK_STOP_PHASE_COMPLETED:
        ESP_LOGI(TAG, "audio_task stop: completed");
        break;
    case AUDIO_TASK_STOP_PHASE_TIMEOUT:
        ESP_LOGW(TAG, "audio_task stop: timeout");
        break;
    default:
        break;
    }
}

static TickType_t audio_task_ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static void audio_task_request_stop_signal(void)
{
    s_stop_requested = true;
    if (s_lifecycle_events != NULL) {
        (void)xEventGroupSetBits(s_lifecycle_events, AUDIO_TASK_LIFECYCLE_STOP_REQUESTED);
    }
}

static bool audio_task_is_stop_requested(void)
{
    if (s_stop_requested) {
        return true;
    }
    EventGroupHandle_t events = s_lifecycle_events;
    if (events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(events);
    return ((bits & AUDIO_TASK_LIFECYCLE_STOP_REQUESTED) != 0U);
}

static esp_err_t audio_task_wait_stopped(TickType_t wait_ticks)
{
    if (wait_ticks == 0) {
        wait_ticks = 1;
    }
    if (s_lifecycle_events != NULL) {
        EventBits_t bits = xEventGroupWaitBits(s_lifecycle_events,
                                               AUDIO_TASK_LIFECYCLE_STOPPED,
                                               pdFALSE,
                                               pdFALSE,
                                               wait_ticks);
        if ((bits & AUDIO_TASK_LIFECYCLE_STOPPED) == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        return ESP_OK;
    }

    TickType_t deadline = xTaskGetTickCount() + wait_ticks;
    while (s_audio_task_handle != NULL && (int32_t)(xTaskGetTickCount() - deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return (s_audio_task_handle == NULL) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void audio_task_process_command(const audio_command_t *cmd)
{
    if (cmd == NULL) {
        return;
    }
    audio_worker_handle_command(cmd);
}

static void audio_task_process_idle_iteration(QueueHandle_t queue)
{
    audio_command_t cmd = { 0 };
    uint32_t recv_timeout_ms = (uint32_t)CONFIG_ORB_QUEUE_RECV_TIMEOUT_MS;
    if (recv_timeout_ms > 100U) {
        recv_timeout_ms = 100U;
    }
    BaseType_t got = xQueueReceive(queue, &cmd, audio_task_ms_to_ticks_min1(recv_timeout_ms));
    if (got == pdTRUE) {
        audio_task_process_command(&cmd);
    }
    audio_worker_poll();
}

static void audio_task_process_active_iteration(QueueHandle_t queue)
{
    audio_command_t cmd = { 0 };
    BaseType_t got = xQueueReceive(queue, &cmd, audio_task_ms_to_ticks_min1(10U));
    if (got == pdTRUE) {
        audio_task_process_command(&cmd);
    }
    audio_worker_poll();
}

static void audio_task_wake(void)
{
    TaskHandle_t handle = s_audio_task_handle;
    if (handle != NULL) {
#if defined(INCLUDE_xTaskAbortDelay) && (INCLUDE_xTaskAbortDelay == 1)
        (void)xTaskAbortDelay(handle);
#else
        xTaskNotifyGive(handle);
        QueueHandle_t queue = app_tasking_get_audio_cmd_queue();
        if (queue != NULL) {
            audio_command_t wake = { .id = AUDIO_CMD_NONE };
            const TickType_t wait_ticks = pdMS_TO_TICKS(10);
            for (uint32_t attempt = 0U; attempt < 3U; ++attempt) {
                if (xQueueSend(queue, &wake, wait_ticks) == pdTRUE) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
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

    while (!audio_task_is_stop_requested()) {
        if (audio_worker_is_playing()) {
            audio_task_process_active_iteration(queue);
        } else {
            audio_task_process_idle_iteration(queue);
        }
    }

    /* Graceful self-owned shutdown path: stop playback and release hardware. */
    audio_command_t stop_cmd = { .id = AUDIO_CMD_STOP };
    audio_worker_handle_command(&stop_cmd);
    (void)audio_output_i2s_stop();

    if (s_lifecycle_events != NULL) {
        (void)xEventGroupSetBits(s_lifecycle_events, AUDIO_TASK_LIFECYCLE_STOPPED);
    }
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
    if (s_lifecycle_events == NULL) {
        s_lifecycle_events = xEventGroupCreate();
        if (s_lifecycle_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_stop_requested = false;
    (void)xEventGroupClearBits(s_lifecycle_events, AUDIO_TASK_LIFECYCLE_STOP_REQUESTED | AUDIO_TASK_LIFECYCLE_STOPPED);

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

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (self == s_audio_task_handle) {
        audio_task_request_stop_signal();
        return ESP_OK;
    }

    audio_task_log_stop_phase(AUDIO_TASK_STOP_PHASE_REQUESTED);
    audio_task_request_stop_signal();
    if (s_lifecycle_events != NULL) {
        (void)xEventGroupClearBits(s_lifecycle_events, AUDIO_TASK_LIFECYCLE_STOPPED);
    }
    audio_task_wake();

    audio_task_log_stop_phase(AUDIO_TASK_STOP_PHASE_WAITING);
    TickType_t stop_wait_ticks = pdMS_TO_TICKS((uint32_t)CONFIG_ORB_AUDIO_STOP_TIMEOUT_MS + 200U);
    esp_err_t wait_err = audio_task_wait_stopped(stop_wait_ticks);
    if (wait_err != ESP_OK) {
        audio_task_log_stop_phase(AUDIO_TASK_STOP_PHASE_TIMEOUT);
        return wait_err;
    }

    audio_task_log_stop_phase(AUDIO_TASK_STOP_PHASE_COMPLETED);
    return ESP_OK;
}
