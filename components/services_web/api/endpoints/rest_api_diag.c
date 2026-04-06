#include "rest_api_modules.h"

#include <stdio.h>
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "log_tags.h"
#include "rest_api_common.h"

static const char *TAG = LOG_TAG_REST;

static esp_err_t diag_log_handler(httpd_req_t *req)
{
    char op[48] = { 0 };
    char result[24] = { 0 };
    char target[160] = { 0 };
    char detail[160] = { 0 };

    if (rest_api_query_value(req, "op", op, sizeof(op)) != ESP_OK) {
        (void)snprintf(op, sizeof(op), "%s", "unknown");
    }
    if (rest_api_query_value(req, "result", result, sizeof(result)) != ESP_OK) {
        (void)snprintf(result, sizeof(result), "%s", "-");
    }
    if (rest_api_query_value(req, "target", target, sizeof(target)) != ESP_OK) {
        (void)snprintf(target, sizeof(target), "%s", "-");
    }
    if (rest_api_query_value(req, "detail", detail, sizeof(detail)) != ESP_OK) {
        (void)snprintf(detail, sizeof(detail), "%s", "-");
    }

    ESP_LOGI(TAG,
             "portal event op=%s result=%s target=%s detail=%s",
             op,
             result,
             target,
             detail);

    return rest_api_send_json(req, "200 OK", "{\"ok\":true}");
}

esp_err_t rest_api_register_diag_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t log_uri = {
        .uri = "/api/diag/log",
        .method = HTTP_POST,
        .handler = diag_log_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &log_uri), TAG, "register diag log failed");
    return ESP_OK;
}

