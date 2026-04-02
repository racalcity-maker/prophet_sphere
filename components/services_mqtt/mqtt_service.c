#include "mqtt_service.h"

#include <stdbool.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_tags.h"
#include "mqtt_topics.h"
#include "sdkconfig.h"
#include "service_lifecycle_guard.h"

static const char *TAG = LOG_TAG_MQTT;
static bool s_started;
static SemaphoreHandle_t s_state_mutex;

esp_err_t mqtt_service_init(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "mqtt init denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CONFIG_ORB_ENABLE_MQTT) {
        ESP_LOGW(TAG, "mqtt disabled");
        return ESP_OK;
    }
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "mqtt initialized base_topic=%s", CONFIG_ORB_MQTT_BASE_TOPIC);
    return ESP_OK;
}

esp_err_t mqtt_service_start(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "mqtt start denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CONFIG_ORB_ENABLE_MQTT) {
        return ESP_OK;
    }
    if (s_state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool should_start = false;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_started) {
        s_started = true;
        should_start = true;
    }
    xSemaphoreGive(s_state_mutex);

    if (!should_start) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "mqtt start, topic command=%s", MQTT_TOPIC_MODE_SWITCH);
    return ESP_OK;
}

esp_err_t mqtt_service_stop(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "mqtt stop denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CONFIG_ORB_ENABLE_MQTT) {
        return ESP_OK;
    }
    if (s_state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool should_stop = false;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_started) {
        s_started = false;
        should_stop = true;
    }
    xSemaphoreGive(s_state_mutex);

    if (!should_stop) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "mqtt stop");
    return ESP_OK;
}
