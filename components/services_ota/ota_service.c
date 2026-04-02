#include "ota_service.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_tags.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_OTA;

static SemaphoreHandle_t s_state_mutex;
static bool s_started;

#ifndef CONFIG_ORB_ENABLE_OTA
#define CONFIG_ORB_ENABLE_OTA 1
#endif
#ifndef CONFIG_ORB_OTA_URL_MAX_LEN
#define CONFIG_ORB_OTA_URL_MAX_LEN 192
#endif
#ifndef CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS
#define CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS 50
#endif

bool ota_service_is_enabled(void)
{
    return CONFIG_ORB_ENABLE_OTA;
}

esp_err_t ota_service_init(void)
{
    if (!CONFIG_ORB_ENABLE_OTA) {
        ESP_LOGW(TAG, "OTA disabled");
        return ESP_OK;
    }

    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        ESP_LOGI(TAG,
                 "ota init running partition label=%s subtype=%d addr=0x%08" PRIx32,
                 running->label,
                 (int)running->subtype,
                 running->address);
    } else {
        ESP_LOGI(TAG, "ota init (running partition unknown)");
    }

    return ESP_OK;
}

esp_err_t ota_service_start(void)
{
    if (!CONFIG_ORB_ENABLE_OTA) {
        return ESP_OK;
    }
    if (s_state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_started) {
        s_started = true;
        ESP_LOGI(TAG, "ota service started");
    }
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t ota_service_stop(void)
{
    if (!CONFIG_ORB_ENABLE_OTA) {
        return ESP_OK;
    }
    if (s_state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_started) {
        s_started = false;
        ESP_LOGI(TAG, "ota service stopped");
    }
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t ota_service_mark_boot_success(void)
{
    if (!CONFIG_ORB_ENABLE_OTA) {
        return ESP_OK;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK || err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

esp_err_t ota_service_request_update(const char *url)
{
    if (!CONFIG_ORB_ENABLE_OTA) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) > (size_t)CONFIG_ORB_OTA_URL_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool started = false;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    started = s_started;
    xSemaphoreGive(s_state_mutex);
    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "OTA request accepted but updater is not implemented yet: %s", url);
    return ESP_ERR_NOT_SUPPORTED;
}
