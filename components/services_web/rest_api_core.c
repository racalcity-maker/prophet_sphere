#include "rest_api_modules.h"

#include <stdio.h>
#include "app_fsm.h"
#include "config_manager.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "log_tags.h"
#include "mode_manager.h"
#include "network_manager.h"
#include "rest_api_common.h"
#include "session_controller.h"

static const char *TAG = LOG_TAG_REST;

static esp_err_t health_handler(httpd_req_t *req)
{
    return rest_api_send_json(req, "200 OK", "{\"ok\":true,\"service\":\"web\"}");
}

static esp_err_t status_handler(httpd_req_t *req)
{
    orb_runtime_config_t cfg = { 0 };
    session_info_t session = { 0 };
    network_status_t net = { 0 };
    (void)config_manager_get_snapshot(&cfg);
    (void)session_controller_get_info(&session);
    (void)network_manager_get_status(&net);

    const char *mode_name = mode_manager_mode_to_str(mode_manager_get_current_mode());
    const char *fsm_name = app_fsm_state_to_str(app_fsm_get_state());
    const char *session_state_name = session_controller_state_to_str(session.state);
    const char *desired_profile_name = network_manager_profile_to_str(net.desired_profile);
    const char *active_profile_name = network_manager_profile_to_str(net.active_profile);
    const char *link_state_name = network_manager_link_state_to_str(net.link_state);
    const char *aura_color = cfg.aura_selected_color[0] == '\0' ? "" : cfg.aura_selected_color;
    const char *sta_ip = net.sta_ip[0] == '\0' ? "" : net.sta_ip;
    const char *ap_ip = net.ap_ip[0] == '\0' ? "" : net.ap_ip;

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
        mode_name,
        fsm_name,
        (unsigned long)session.session_id,
        session_state_name,
        session.active ? "true" : "false",
        cfg.led_brightness,
        cfg.audio_volume,
        cfg.network_enabled ? "true" : "false",
        cfg.mqtt_enabled ? "true" : "false",
        cfg.ai_enabled ? "true" : "false",
        cfg.web_enabled ? "true" : "false",
        config_manager_offline_submode_to_str(cfg.offline_submode),
        (unsigned long)cfg.aura_gap_ms,
        aura_color,
        (unsigned)cfg.hybrid_reject_threshold_permille,
        (unsigned long)cfg.hybrid_mic_capture_ms,
        net.network_up ? "true" : "false",
        link_state_name,
        desired_profile_name,
        active_profile_name,
        sta_ip,
        ap_ip);
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
