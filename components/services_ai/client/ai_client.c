#include "ai_client.h"

#include <stdio.h>
#include "sdkconfig.h"
#include "ai_task.h"
#include "app_tasking.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "prompt_engine.h"

static const char *TAG = LOG_TAG_AI;

esp_err_t ai_client_init(void)
{
    if (!CONFIG_ORB_ENABLE_AI) {
        ESP_LOGW(TAG, "AI disabled by config");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "initialized");
    ESP_RETURN_ON_ERROR(prompt_engine_init(), TAG, "prompt engine init failed");
    return ESP_OK;
}

esp_err_t ai_client_start_task(void)
{
    return ai_task_start();
}

esp_err_t ai_client_stop_task(void)
{
    if (!CONFIG_ORB_ENABLE_AI) {
        return ESP_OK;
    }
    (void)ai_client_cancel(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(20));
    return ai_task_stop();
}

esp_err_t ai_client_request(uint32_t request_id, const char *prompt, uint32_t timeout_ms)
{
    if (!CONFIG_ORB_ENABLE_AI) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ai_command_t cmd = { 0 };
    char composed[sizeof(cmd.payload.request.prompt)] = { 0 };
    cmd.id = AI_CMD_REQUEST;
    cmd.payload.request.request_id = request_id;
    ESP_RETURN_ON_ERROR(prompt_engine_compose(prompt, composed, sizeof(composed)), TAG, "prompt compose failed");
    (void)snprintf(cmd.payload.request.prompt, sizeof(cmd.payload.request.prompt), "%s", composed);
    return app_tasking_send_ai_command(&cmd, timeout_ms);
}

esp_err_t ai_client_cancel(uint32_t timeout_ms)
{
    ai_command_t cmd = { .id = AI_CMD_CANCEL };
    return app_tasking_send_ai_command(&cmd, timeout_ms);
}
