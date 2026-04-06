#include "ai_task.h"

#include <inttypes.h>
#include "sdkconfig.h"
#include "app_tasking.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AI;
static TaskHandle_t s_ai_task_handle;

static void ai_task_entry(void *arg)
{
    (void)arg;
    QueueHandle_t queue = app_tasking_get_ai_cmd_queue();
    if (queue == NULL) {
        ESP_LOGE(TAG, "ai_cmd_queue is not initialized");
        s_ai_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "ai_task started");
    while (true) {
        ai_command_t cmd = { 0 };
        BaseType_t got = xQueueReceive(queue, &cmd, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_RECV_TIMEOUT_MS));
        if (got != pdTRUE) {
            continue;
        }

        switch (cmd.id) {
        case AI_CMD_REQUEST: {
            uint32_t request_id = cmd.payload.request.request_id;
            ESP_LOGI(TAG, "REQUEST id=%" PRIu32 " prompt=\"%s\"", request_id, cmd.payload.request.prompt);

            app_event_t ready = { 0 };
            ready.id = APP_EVENT_AI_RESPONSE_READY;
            ready.source = APP_EVENT_SOURCE_AI;
            ready.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            ready.payload.scalar.value = request_id;
            (void)app_tasking_post_event(&ready, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
            break;
        }
        case AI_CMD_CANCEL:
            ESP_LOGI(TAG, "CANCEL");
            break;
        case AI_CMD_NONE:
        default:
            ESP_LOGW(TAG, "unknown command id=%d", (int)cmd.id);
            break;
        }
    }
}

esp_err_t ai_task_start(void)
{
    if (!CONFIG_ORB_ENABLE_AI) {
        return ESP_OK;
    }
    if (s_ai_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(ai_task_entry,
                                "ai_task",
                                CONFIG_ORB_AI_TASK_STACK_SIZE,
                                NULL,
                                CONFIG_ORB_AI_TASK_PRIORITY,
                                &s_ai_task_handle);
    if (ok != pdPASS) {
        s_ai_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "task created prio=%d stack=%d", CONFIG_ORB_AI_TASK_PRIORITY, CONFIG_ORB_AI_TASK_STACK_SIZE);
    return ESP_OK;
}

esp_err_t ai_task_stop(void)
{
    if (s_ai_task_handle == NULL) {
        return ESP_OK;
    }

    TaskHandle_t handle = s_ai_task_handle;
    s_ai_task_handle = NULL;
    vTaskDelete(handle);
    ESP_LOGI(TAG, "ai_task stopped");
    return ESP_OK;
}
