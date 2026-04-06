#include "rest_api_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_tags.h"
#include "sdkconfig.h"

static SemaphoreHandle_t s_web_buffer_mutex;
static char *s_shared_json_buffer;
static size_t s_shared_json_capacity;

static const char *TAG = LOG_TAG_REST;

#ifndef CONFIG_ORB_WEB_SHARED_JSON_BUF_SIZE
#define CONFIG_ORB_WEB_SHARED_JSON_BUF_SIZE 8192
#endif

#ifndef CONFIG_ORB_WEB_USE_PSRAM_BUFFERS
#define CONFIG_ORB_WEB_USE_PSRAM_BUFFERS 1
#endif

static void *alloc_web_buffer(size_t size, bool *from_psram)
{
    if (from_psram != NULL) {
        *from_psram = false;
    }

#if CONFIG_ORB_WEB_USE_PSRAM_BUFFERS
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != NULL) {
        if (from_psram != NULL) {
            *from_psram = true;
        }
        return ptr;
    }
#endif

    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static TickType_t lock_timeout_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    return ticks > 0 ? ticks : 1;
}

esp_err_t rest_api_common_init(void)
{
    if (s_web_buffer_mutex == NULL) {
        s_web_buffer_mutex = xSemaphoreCreateMutex();
        if (s_web_buffer_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_shared_json_buffer == NULL) {
        bool json_from_psram = false;

        if (s_shared_json_buffer == NULL) {
            s_shared_json_capacity = CONFIG_ORB_WEB_SHARED_JSON_BUF_SIZE;
            s_shared_json_buffer = (char *)alloc_web_buffer(s_shared_json_capacity, &json_from_psram);
            if (s_shared_json_buffer == NULL) {
                return ESP_ERR_NO_MEM;
            }
            memset(s_shared_json_buffer, 0, s_shared_json_capacity);
            ESP_LOGI(TAG,
                     "web shared JSON buffer: %u bytes (%s)",
                     (unsigned)s_shared_json_capacity,
                     json_from_psram ? "psram" : "internal");
        }
    }

    return ESP_OK;
}

esp_err_t rest_api_web_buffer_lock(void)
{
    if (rest_api_common_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_web_buffer_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_web_buffer_mutex, lock_timeout_ticks()) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void rest_api_web_buffer_unlock(void)
{
    if (s_web_buffer_mutex != NULL) {
        xSemaphoreGive(s_web_buffer_mutex);
    }
}

char *rest_api_shared_json_buffer(size_t *capacity)
{
    if (capacity != NULL) {
        *capacity = s_shared_json_capacity;
    }
    return s_shared_json_buffer;
}

void rest_api_set_json_headers(httpd_req_t *req)
{
    if (req == NULL) {
        return;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

esp_err_t rest_api_send_json(httpd_req_t *req, const char *status, const char *json)
{
    if (req == NULL || status == NULL || json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rest_api_set_json_headers(req);
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, json);
}

esp_err_t rest_api_send_error_json(httpd_req_t *req, const char *status, const char *message)
{
    if (req == NULL || status == NULL || message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char body[192];
    (void)snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message);
    return rest_api_send_json(req, status, body);
}

bool rest_api_parse_u32(const char *text, uint32_t *out)
{
    if (text == NULL || text[0] == '\0' || out == NULL) {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

bool rest_api_parse_bool_text(const char *text, bool *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    if (strcmp(text, "1") == 0 || strcmp(text, "true") == 0 || strcmp(text, "on") == 0 || strcmp(text, "yes") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(text, "0") == 0 || strcmp(text, "false") == 0 || strcmp(text, "off") == 0 || strcmp(text, "no") == 0) {
        *out = false;
        return true;
    }
    return false;
}

esp_err_t rest_api_query_value(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    if (req == NULL || key == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t query_len = (size_t)httpd_req_get_url_query_len(req) + 1U;
    if (query_len <= 1U) {
        return ESP_ERR_NOT_FOUND;
    }

    char query[REST_API_QUERY_BUF_LEN];
    if (query_len > sizeof(query)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (httpd_query_key_value(query, key, out, out_len) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}
