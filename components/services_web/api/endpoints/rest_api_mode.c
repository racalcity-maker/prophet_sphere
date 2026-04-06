#include "rest_api_modules.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_http_server.h"
#include "log_tags.h"
#include "mode_manager.h"
#include "rest_api.h"
#include "rest_api_common.h"

static const char *TAG = LOG_TAG_REST;

static bool mode_from_text(const char *text, orb_mode_t *mode)
{
    if (text == NULL || mode == NULL) {
        return false;
    }
    if (strcmp(text, "offline_scripted") == 0) {
        *mode = ORB_MODE_OFFLINE_SCRIPTED;
        return true;
    }
    if (strcmp(text, "hybrid_ai") == 0) {
        *mode = ORB_MODE_HYBRID_AI;
        return true;
    }
    if (strcmp(text, "installation_slave") == 0) {
        *mode = ORB_MODE_INSTALLATION_SLAVE;
        return true;
    }
    return false;
}

static esp_err_t mode_switch_handler(httpd_req_t *req)
{
    char mode_text[32];
    if (rest_api_query_value(req, "mode", mode_text, sizeof(mode_text)) != ESP_OK) {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_mode");
    }

    orb_mode_t target = ORB_MODE_NONE;
    if (!mode_from_text(mode_text, &target)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_mode");
    }

    const orb_mode_t current = mode_manager_get_current_mode();
    /* Offline portal must not switch away from offline.
     * AP transport is mode-scoped and dropping it mid-session breaks UX. */
    if (current == ORB_MODE_OFFLINE_SCRIPTED && target != ORB_MODE_OFFLINE_SCRIPTED) {
        return rest_api_send_error_json(req, "409 Conflict", "mode_switch_blocked_in_offline");
    }

    esp_err_t err = rest_api_request_mode_switch(target);
    if (err != ESP_OK) {
        return rest_api_send_error_json(req, "500 Internal Server Error", "mode_switch_request_failed");
    }

    char json[160];
    (void)snprintf(json, sizeof(json), "{\"ok\":true,\"requested_mode\":\"%s\"}", mode_manager_mode_to_str(target));
    return rest_api_send_json(req, "200 OK", json);
}

esp_err_t rest_api_register_mode_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t mode_switch = {
        .uri = "/api/mode/switch",
        .method = HTTP_POST,
        .handler = mode_switch_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &mode_switch), TAG, "register mode switch failed");
    return ESP_OK;
}
