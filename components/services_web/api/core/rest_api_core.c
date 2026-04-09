#include "rest_api_modules.h"

#include <stdio.h>
#include "app_api.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "log_tags.h"
#include "rest_api_common.h"

static const char *TAG = LOG_TAG_REST;

static esp_err_t health_handler(httpd_req_t *req)
{
    return rest_api_send_json(req, "200 OK", "{\"ok\":true,\"service\":\"web\"}");
}

static esp_err_t status_handler(httpd_req_t *req)
{
    app_api_status_snapshot_t status = { 0 };
    (void)app_api_get_status_snapshot(&status);

    esp_err_t buf_err = rest_api_web_buffer_lock();
    if (buf_err != ESP_OK) {
        return rest_api_send_error_json(req, "500 Internal Server Error", "status_buf_lock_failed");
    }
    size_t json_cap = 0U;
    char *json = rest_api_shared_json_buffer(&json_cap);
    if (json == NULL || json_cap == 0U) {
        rest_api_web_buffer_unlock();
        return rest_api_send_error_json(req, "500 Internal Server Error", "status_no_mem");
    }
    json[0] = '\0';

    int n = snprintf(
        json,
        json_cap,
        "{\"ok\":true,\"mode\":\"%s\",\"fsm\":\"%s\",\"session\":{\"id\":%lu,\"state\":\"%s\",\"active\":%s},"
        "\"config\":{\"brightness\":%u,\"volume\":%u,\"network\":%s,\"mqtt\":%s,\"ai\":%s,\"web\":%s,"
        "\"offline_submode\":\"%s\",\"aura_gap_ms\":%lu,\"aura_selected_color\":\"%s\","
        "\"hybrid_reject_threshold_permille\":%u,\"hybrid_mic_capture_ms\":%lu},"
        "\"network\":{\"up\":%s,\"state\":\"%s\",\"desired\":\"%s\",\"active\":\"%s\",\"sta_ip\":\"%s\",\"ap_ip\":\"%s\"}}",
        status.mode_name,
        status.fsm_name,
        (unsigned long)status.session_id,
        status.session_state_name,
        status.session_active ? "true" : "false",
        status.brightness,
        status.volume,
        status.network_enabled ? "true" : "false",
        status.mqtt_enabled ? "true" : "false",
        status.ai_enabled ? "true" : "false",
        status.web_enabled ? "true" : "false",
        status.offline_submode_name,
        (unsigned long)status.aura_gap_ms,
        status.aura_selected_color,
        (unsigned)status.hybrid_reject_threshold_permille,
        (unsigned long)status.hybrid_mic_capture_ms,
        status.network_up ? "true" : "false",
        status.link_state_name,
        status.desired_profile_name,
        status.active_profile_name,
        status.sta_ip,
        status.ap_ip);
    if (n <= 0 || (size_t)n >= json_cap) {
        rest_api_web_buffer_unlock();
        return rest_api_send_error_json(req, "500 Internal Server Error", "status_serialize_failed");
    }

    esp_err_t send_err = rest_api_send_json(req, "200 OK", json);
    rest_api_web_buffer_unlock();
    return send_err;
}

esp_err_t rest_api_register_core_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t health = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &health), TAG, "register health failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &status), TAG, "register status failed");
    return ESP_OK;
}
