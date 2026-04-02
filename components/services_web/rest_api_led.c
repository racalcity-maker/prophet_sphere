#include "rest_api_modules.h"

#include <stdio.h>
#include "config_manager.h"
#include "control_dispatch.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "log_tags.h"
#include "rest_api_common.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_REST;

static esp_err_t led_scene_handler(httpd_req_t *req)
{
    char scene_text[16];
    char duration_text[16];

    if (rest_api_query_value(req, "scene", scene_text, sizeof(scene_text)) != ESP_OK) {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_scene");
    }

    uint32_t scene_id = 0;
    if (!rest_api_parse_u32(scene_text, &scene_id)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_scene");
    }

    uint32_t duration_ms = 0;
    if (rest_api_query_value(req, "duration_ms", duration_text, sizeof(duration_text)) == ESP_OK) {
        if (!rest_api_parse_u32(duration_text, &duration_ms)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_duration_ms");
        }
    }

    esp_err_t err = control_dispatch_queue_led_scene(scene_id, duration_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led scene failed: %s", esp_err_to_name(err));
        return rest_api_send_error_json(req, "500 Internal Server Error", "led_scene_failed");
    }

    char json[192];
    (void)snprintf(json,
                   sizeof(json),
                   "{\"ok\":true,\"scene\":%lu,\"duration_ms\":%lu}",
                   (unsigned long)scene_id,
                   (unsigned long)duration_ms);
    return rest_api_send_json(req, "200 OK", json);
}

static esp_err_t led_brightness_handler(httpd_req_t *req)
{
    char value_text[16];
    if (rest_api_query_value(req, "value", value_text, sizeof(value_text)) != ESP_OK) {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_value");
    }

    uint32_t value = 0;
    if (!rest_api_parse_u32(value_text, &value) || value > 255U) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_value");
    }

    esp_err_t err = control_dispatch_queue_led_brightness((uint8_t)value);
    if (err != ESP_OK) {
        return rest_api_send_error_json(req, "500 Internal Server Error", "led_brightness_failed");
    }
    (void)config_manager_set_led_brightness((uint8_t)value);

    char json[80];
    (void)snprintf(json, sizeof(json), "{\"ok\":true,\"brightness\":%lu}", (unsigned long)value);
    return rest_api_send_json(req, "200 OK", json);
}

esp_err_t rest_api_register_led_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t scene = {
        .uri = "/api/led/scene",
        .method = HTTP_POST,
        .handler = led_scene_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t brightness = {
        .uri = "/api/led/brightness",
        .method = HTTP_POST,
        .handler = led_brightness_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &scene), TAG, "register led scene failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &brightness), TAG, "register led brightness failed");
    return ESP_OK;
}
