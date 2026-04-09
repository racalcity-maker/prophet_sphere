#include "rest_api_modules.h"

#include <stdio.h>
#include <string.h>
#include "app_api.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "log_tags.h"
#include "rest_api_common.h"

static const char *TAG = LOG_TAG_REST;

static esp_err_t send_network_status(httpd_req_t *req)
{
    app_api_network_status_t status = { 0 };
    (void)app_api_get_network_status(&status);

    char json[640];
    (void)snprintf(json,
                   sizeof(json),
                   "{\"ok\":true,"
                   "\"desired\":\"%s\","
                   "\"active\":\"%s\","
                   "\"link_state\":\"%s\","
                   "\"up\":%s,"
                   "\"sta_ssid\":\"%s\","
                   "\"sta_password_set\":%s,"
                   "\"sta_ip\":\"%s\","
                   "\"ap_ip\":\"%s\"}",
                   status.desired_profile,
                   status.active_profile,
                   status.link_state,
                   status.network_up ? "true" : "false",
                   status.sta_ssid,
                   status.sta_password_set ? "true" : "false",
                   status.sta_ip,
                   status.ap_ip);
    return rest_api_send_json(req, "200 OK", json);
}

static esp_err_t network_status_handler(httpd_req_t *req)
{
    return send_network_status(req);
}

static esp_err_t network_config_handler(httpd_req_t *req)
{
    char ssid[APP_API_STA_SSID_MAX] = { 0 };
    char password[APP_API_STA_PASSWORD_MAX] = { 0 };
    char value[16] = { 0 };
    bool persist = true;

    if (rest_api_query_value(req, "sta_ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_sta_ssid");
    }
    if (rest_api_query_value(req, "sta_password", password, sizeof(password)) != ESP_OK) {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_sta_password");
    }
    if (rest_api_query_value(req, "save", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_bool_text(value, &persist)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_save");
        }
    }

    esp_err_t err = app_api_apply_sta_credentials_and_reconfigure(ssid, password, persist);
    if (err != ESP_OK) {
        return rest_api_send_error_json(req, "500 Internal Server Error", "apply_sta_credentials_failed");
    }

    return send_network_status(req);
}

esp_err_t rest_api_register_network_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t network_status = {
        .uri = "/api/network/status",
        .method = HTTP_GET,
        .handler = network_status_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t network_cfg = {
        .uri = "/api/network/config",
        .method = HTTP_POST,
        .handler = network_config_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &network_status), TAG, "register network status failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &network_cfg), TAG, "register network config failed");
    return ESP_OK;
}
