#include "led_task.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_task_internal.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_LED;

#ifndef CONFIG_ORB_LED_STOP_TIMEOUT_MS
#define CONFIG_ORB_LED_STOP_TIMEOUT_MS 300
#endif

esp_err_t led_task_start(void)
{
    if (s_led_task_handle != NULL) {
        if (s_stop_requested) {
            ESP_LOGW(TAG, "start requested while stop is in progress");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }
    s_stop_requested = false;

    BaseType_t ok = xTaskCreate(led_task_entry,
                                "led_task",
                                CONFIG_ORB_LED_TASK_STACK_SIZE,
                                NULL,
                                CONFIG_ORB_LED_TASK_PRIORITY,
                                &s_led_task_handle);
    if (ok != pdPASS) {
        s_led_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "task created prio=%d stack=%d", CONFIG_ORB_LED_TASK_PRIORITY, CONFIG_ORB_LED_TASK_STACK_SIZE);
    return ESP_OK;
}

esp_err_t led_task_stop(void)
{
    if (s_led_task_handle == NULL) {
        return ESP_OK;
    }

    s_stop_requested = true;
    /* Lifecycle stop is independent of content-clear command path. */
    led_task_wake();

    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)CONFIG_ORB_LED_STOP_TIMEOUT_MS + 200U);
    while (s_led_task_handle != NULL) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGW(TAG, "led_task graceful stop timeout (task still running)");
            return ESP_ERR_TIMEOUT;
        }
        led_task_wake();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}
