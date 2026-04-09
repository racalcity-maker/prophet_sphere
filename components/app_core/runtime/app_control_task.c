#include "app_control_task.h"

#include "sdkconfig.h"
#include "app_events.h"
#include "app_fsm.h"
#include "app_tasking.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_APP_TASKING;
static TaskHandle_t s_app_control_task_handle;
static QueueHandle_t s_app_event_queue;
#define APP_CONTROL_TASK_STACK_MIN_SAFE 12288U

static void drain_pending_timer_events(void)
{
    app_event_t event = { 0 };
    while (app_tasking_take_pending_timer_event(&event)) {
        esp_err_t err = app_fsm_handle_event(&event);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "FSM rejected deferred %s: %s", app_event_id_to_str(event.id), esp_err_to_name(err));
        }
    }
}

static void app_control_task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "app_control_task started");

    while (true) {
        app_event_t event = { 0 };
        BaseType_t got =
            xQueueReceive(s_app_event_queue, &event, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_RECV_TIMEOUT_MS));

        if (got == pdTRUE) {
            esp_err_t err = app_fsm_handle_event(&event);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "FSM rejected event %s: %s", app_event_id_to_str(event.id), esp_err_to_name(err));
            }
        }

        drain_pending_timer_events();
    }
}

esp_err_t app_control_task_start(QueueHandle_t app_event_queue)
{
    if (app_event_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_app_control_task_handle != NULL) {
        return ESP_OK;
    }

    s_app_event_queue = app_event_queue;
    uint32_t stack_size = (uint32_t)CONFIG_ORB_APP_CONTROL_TASK_STACK_SIZE;
    if (stack_size < APP_CONTROL_TASK_STACK_MIN_SAFE) {
        ESP_LOGW(TAG,
                 "app_control_task stack %lu too small; using min safe %u",
                 (unsigned long)stack_size,
                 (unsigned)APP_CONTROL_TASK_STACK_MIN_SAFE);
        stack_size = APP_CONTROL_TASK_STACK_MIN_SAFE;
    }
    BaseType_t ok = xTaskCreate(app_control_task_entry,
                                "app_control_task",
                                stack_size,
                                NULL,
                                CONFIG_ORB_APP_CONTROL_TASK_PRIORITY,
                                &s_app_control_task_handle);
    if (ok != pdPASS) {
        s_app_control_task_handle = NULL;
        s_app_event_queue = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "app_control_task created prio=%d stack=%lu",
             CONFIG_ORB_APP_CONTROL_TASK_PRIORITY,
             (unsigned long)stack_size);
    return ESP_OK;
}
