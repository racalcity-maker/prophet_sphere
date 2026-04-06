#include "rest_api_modules.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "log_tags.h"
#include "rest_api_common.h"

static const char *TAG = LOG_TAG_REST;

#define SERVER_TTS_PROXY_FORM_MAX 4096
#define SERVER_TTS_PROXY_TARGET_MAX 192
#define SERVER_TTS_PROXY_TOKEN_MAX 192
#define SERVER_TTS_PROXY_OP_MAX 24
#define SERVER_TTS_PROXY_PATCH_MAX 3072
#define SERVER_TTS_PROXY_URL_MAX 320
#define SERVER_TTS_PROXY_RESP_MAX 8192

static esp_err_t proxy_read_form_body(httpd_req_t *req, char *out, size_t out_len)
{
    if (req == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (req->content_len <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t)req->content_len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    int remaining = req->content_len;
    int total = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, out + total, remaining);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        total += n;
        remaining -= n;
    }
    out[total] = '\0';
    return ESP_OK;
}

static esp_err_t proxy_get_param(httpd_req_t *req, const char *form_body, const char *key, char *out, size_t out_len)
{
    if (req == NULL || key == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t qerr = rest_api_query_value(req, key, out, out_len);
    if (qerr == ESP_OK) {
        return ESP_OK;
    }
    if (form_body != NULL && form_body[0] != '\0') {
        esp_err_t ferr = httpd_query_key_value(form_body, key, out, out_len);
        if (ferr == ESP_OK) {
            return ESP_OK;
        }
        if (ferr == ESP_ERR_HTTPD_RESULT_TRUNC) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static int proxy_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return 10 + (int)(c - 'a');
    }
    return -1;
}

static bool proxy_url_decode_inplace(char *text)
{
    if (text == NULL) {
        return false;
    }
    char *src = text;
    char *dst = text;
    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        if (*src == '%') {
            if (src[1] == '\0' || src[2] == '\0') {
                return false;
            }
            int hi = proxy_hex_nibble(src[1]);
            int lo = proxy_hex_nibble(src[2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    return true;
}

static esp_err_t proxy_read_upstream_response(
    esp_http_client_handle_t client,
    char *out,
    size_t out_len
)
{
    if (client == NULL || out == NULL || out_len < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t total = 0U;
    out[0] = '\0';
    while (total < out_len - 1U) {
        int n = esp_http_client_read(client, out + total, (int)(out_len - 1U - total));
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    out[total] = '\0';
    return ESP_OK;
}

static esp_err_t proxy_call_upstream(
    const char *url,
    const char *method,
    const char *token,
    const char *json_body,
    int *status_out,
    char *response_out,
    size_t response_out_len
)
{
    if (url == NULL || method == NULL || status_out == NULL || response_out == NULL || response_out_len < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 9000,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, (strcmp(method, "POST") == 0) ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (token != NULL && token[0] != '\0') {
        esp_http_client_set_header(client, "X-Orb-Token", token);
    }
    const bool is_post = (strcmp(method, "POST") == 0);
    const char *body = (json_body != NULL) ? json_body : "{}";
    int body_len = is_post ? (int)strlen(body) : 0;

    if (is_post) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    if (is_post && body_len > 0) {
        int written = esp_http_client_write(client, body, body_len);
        if (written < 0 || written != body_len) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    int64_t headers_ok = esp_http_client_fetch_headers(client);
    if (headers_ok < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    *status_out = esp_http_client_get_status_code(client);
    esp_err_t read_err = proxy_read_upstream_response(client, response_out, response_out_len);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return read_err;
}

static esp_err_t server_tts_proxy_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    char *form_body = NULL;
    char *patch = NULL;
    char *upstream_resp = NULL;

    form_body = (char *)heap_caps_malloc(SERVER_TTS_PROXY_FORM_MAX + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (form_body == NULL) {
        form_body = (char *)heap_caps_malloc(SERVER_TTS_PROXY_FORM_MAX + 1U, MALLOC_CAP_8BIT);
    }
    patch = (char *)heap_caps_malloc(SERVER_TTS_PROXY_PATCH_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (patch == NULL) {
        patch = (char *)heap_caps_malloc(SERVER_TTS_PROXY_PATCH_MAX, MALLOC_CAP_8BIT);
    }
    upstream_resp = (char *)heap_caps_malloc(SERVER_TTS_PROXY_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (upstream_resp == NULL) {
        upstream_resp = (char *)heap_caps_malloc(SERVER_TTS_PROXY_RESP_MAX, MALLOC_CAP_8BIT);
    }

    if (form_body == NULL || patch == NULL || upstream_resp == NULL) {
        ret = rest_api_send_error_json(req, "500 Internal Server Error", "proxy_no_mem");
        goto cleanup;
    }

    form_body[0] = '\0';
    patch[0] = '\0';
    upstream_resp[0] = '\0';

    esp_err_t body_err = proxy_read_form_body(req, form_body, SERVER_TTS_PROXY_FORM_MAX + 1U);
    if (body_err != ESP_OK && body_err != ESP_ERR_NOT_FOUND) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_request_body");
        goto cleanup;
    }

    char op[SERVER_TTS_PROXY_OP_MAX] = { 0 };
    char target[SERVER_TTS_PROXY_TARGET_MAX] = { 0 };
    char token[SERVER_TTS_PROXY_TOKEN_MAX] = { 0 };

    if (proxy_get_param(req, form_body, "op", op, sizeof(op)) != ESP_OK || op[0] == '\0') {
        ret = rest_api_send_error_json(req, "400 Bad Request", "missing_op");
        goto cleanup;
    }
    if (proxy_get_param(req, form_body, "target", target, sizeof(target)) != ESP_OK || target[0] == '\0') {
        ret = rest_api_send_error_json(req, "400 Bad Request", "missing_target");
        goto cleanup;
    }
    (void)proxy_get_param(req, form_body, "token", token, sizeof(token));
    (void)proxy_get_param(req, form_body, "patch", patch, SERVER_TTS_PROXY_PATCH_MAX);

    if (!proxy_url_decode_inplace(op) || !proxy_url_decode_inplace(target) ||
        !proxy_url_decode_inplace(token) || !proxy_url_decode_inplace(patch)) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_encoding");
        goto cleanup;
    }

    const char *method = "GET";
    const char *path = "/api/tts/config";
    const char *json_body = NULL;
    if (strcmp(op, "load") == 0) {
        method = "GET";
        path = "/api/tts/config";
    } else if (strcmp(op, "apply") == 0) {
        method = "POST";
        path = "/api/tts/config";
        json_body = (patch[0] != '\0') ? patch : "{}";
    } else if (strcmp(op, "save") == 0) {
        method = "POST";
        path = "/api/tts/config/save";
        json_body = "{}";
    } else if (strcmp(op, "reload") == 0) {
        method = "POST";
        path = "/api/tts/config/reload";
        json_body = "{}";
    } else if (strcmp(op, "load_voices") == 0) {
        method = "GET";
        path = "/api/tts/voices";
    } else {
        ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_op");
        goto cleanup;
    }

    size_t tlen = strlen(target);
    while (tlen > 0U && target[tlen - 1U] == '/') {
        target[tlen - 1U] = '\0';
        tlen--;
    }
    if (strncmp(target, "http://", 7) != 0 && strncmp(target, "https://", 8) != 0) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_target_scheme");
        goto cleanup;
    }

    char url[SERVER_TTS_PROXY_URL_MAX];
    int n = snprintf(url, sizeof(url), "%s%s", target, path);
    if (n <= 0 || (size_t)n >= sizeof(url)) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "target_url_too_long");
        goto cleanup;
    }

    ESP_LOGI(TAG, "server_tts proxy op=%s method=%s url=%s", op, method, url);

    int upstream_status = 0;
    esp_err_t call_err = proxy_call_upstream(
        url,
        method,
        token,
        json_body,
        &upstream_status,
        upstream_resp,
        SERVER_TTS_PROXY_RESP_MAX
    );
    if (call_err != ESP_OK) {
        ESP_LOGW(TAG, "server_tts proxy transport failed: %s", esp_err_to_name(call_err));
        ret = rest_api_send_error_json(req, "502 Bad Gateway", "upstream_unreachable");
        goto cleanup;
    }

    if (upstream_status < 200 || upstream_status >= 300) {
        ESP_LOGW(TAG, "server_tts proxy upstream status=%d", upstream_status);
        char err_json[320];
        (void)snprintf(err_json,
                       sizeof(err_json),
                       "{\"ok\":false,\"error\":\"upstream_http_%d\",\"upstream_bytes\":%u}",
                       upstream_status,
                       (unsigned)strlen(upstream_resp));
        ret = rest_api_send_json(req, "502 Bad Gateway", err_json);
        goto cleanup;
    }

    if (upstream_resp[0] == '\0') {
        ret = rest_api_send_json(req, "200 OK", "{\"ok\":false,\"error\":\"empty_upstream_response\"}");
        goto cleanup;
    }
    ESP_LOGI(TAG, "server_tts proxy upstream ok status=%d bytes=%u",
             upstream_status, (unsigned)strlen(upstream_resp));
    ret = rest_api_send_json(req, "200 OK", upstream_resp);

cleanup:
    if (upstream_resp != NULL) {
        free(upstream_resp);
    }
    if (patch != NULL) {
        free(patch);
    }
    if (form_body != NULL) {
        free(form_body);
    }
    return ret;
}

esp_err_t rest_api_register_server_tts_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t proxy_uri = {
        .uri = "/api/server_tts/proxy",
        .method = HTTP_POST,
        .handler = server_tts_proxy_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &proxy_uri), TAG, "register server_tts proxy failed");
    return ESP_OK;
}
