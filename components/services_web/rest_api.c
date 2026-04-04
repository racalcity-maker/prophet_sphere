#include "rest_api.h"

#include "esp_check.h"
#include "esp_log.h"
#include "log_tags.h"
#include "mode_manager.h"
#include "rest_api_common.h"
#include "rest_api_modules.h"

static const char *TAG = LOG_TAG_REST;

static esp_err_t register_handlers(httpd_handle_t server)
{
    ESP_RETURN_ON_ERROR(web_portal_register_http_handlers(server), TAG, "portal handlers");
    ESP_RETURN_ON_ERROR(rest_api_register_core_handlers(server), TAG, "core handlers");
    ESP_RETURN_ON_ERROR(rest_api_register_mode_handlers(server), TAG, "mode handlers");
    ESP_RETURN_ON_ERROR(rest_api_register_led_handlers(server), TAG, "led handlers");
    ESP_RETURN_ON_ERROR(rest_api_register_audio_handlers(server), TAG, "audio handlers");
    ESP_RETURN_ON_ERROR(rest_api_register_config_handlers(server), TAG, "config handlers");
    ESP_RETURN_ON_ERROR(rest_api_register_network_handlers(server), TAG, "network handlers");
    ESP_RETURN_ON_ERROR(rest_api_register_offline_handlers(server), TAG, "offline handlers");
    ESP_RETURN_ON_ERROR(rest_api_register_talk_handlers(server), TAG, "talk handlers");
    ESP_LOGI(TAG, "all REST handlers registered");
    return ESP_OK;
}

esp_err_t rest_api_init(void)
{
    ESP_RETURN_ON_ERROR(rest_api_common_init(), TAG, "common init failed");
    ESP_LOGI(TAG, "rest api initialized");
    return ESP_OK;
}

esp_err_t rest_api_request_mode_switch(orb_mode_t target_mode)
{
    esp_err_t err = mode_manager_request_switch(target_mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mode switch request failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t rest_api_register_http_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return register_handlers(server);
}
