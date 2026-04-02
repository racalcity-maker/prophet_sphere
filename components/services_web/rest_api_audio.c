#include "rest_api_modules.h"

#include <stdio.h>
#include "control_dispatch.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "log_tags.h"
#include "rest_api_common.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_REST;

static esp_err_t audio_play_handler(httpd_req_t *req)
{
    char asset_text[16];
    if (rest_api_query_value(req, "asset", asset_text, sizeof(asset_text)) != ESP_OK) {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_asset");
    }

    uint32_t asset_id = 0;
    if (!rest_api_parse_u32(asset_text, &asset_id) || asset_id == 0U) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_asset");
    }

    esp_err_t err = control_dispatch_queue_audio_asset(asset_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio play failed: %s", esp_err_to_name(err));
        return rest_api_send_error_json(req, "500 Internal Server Error", "audio_play_failed");
    }

    char json[80];
    (void)snprintf(json, sizeof(json), "{\"ok\":true,\"asset\":%lu}", (unsigned long)asset_id);
    return rest_api_send_json(req, "200 OK", json);
}

static esp_err_t audio_stop_handler(httpd_req_t *req)
{
    (void)req;
    esp_err_t err = control_dispatch_queue_audio_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio stop failed: %s", esp_err_to_name(err));
        return rest_api_send_error_json(req, "500 Internal Server Error", "audio_stop_failed");
    }
    return rest_api_send_json(req, "200 OK", "{\"ok\":true}");
}

esp_err_t rest_api_register_audio_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t play = {
        .uri = "/api/audio/play",
        .method = HTTP_POST,
        .handler = audio_play_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t stop = {
        .uri = "/api/audio/stop",
        .method = HTTP_POST,
        .handler = audio_stop_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &play), TAG, "register audio play failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &stop), TAG, "register audio stop failed");
    return ESP_OK;
}
