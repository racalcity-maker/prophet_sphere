#include "web_server.h"

#include <stdbool.h>
#include "esp_check.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_tags.h"
#include "rest_api.h"
#include "sdkconfig.h"
#include "service_lifecycle_guard.h"

static const char *TAG = LOG_TAG_WEB;

extern const unsigned char _binary_servercert_pem_start[] asm("_binary_servercert_pem_start");
extern const unsigned char _binary_servercert_pem_end[] asm("_binary_servercert_pem_end");
extern const unsigned char _binary_prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
extern const unsigned char _binary_prvtkey_pem_end[] asm("_binary_prvtkey_pem_end");

#ifndef CONFIG_ORB_WEB_MAX_URI_HANDLERS
#define CONFIG_ORB_WEB_MAX_URI_HANDLERS 40
#endif
#ifndef CONFIG_ORB_WEB_SERVER_STACK_SIZE
#define CONFIG_ORB_WEB_SERVER_STACK_SIZE 8192
#endif
#ifndef CONFIG_ORB_WEB_QUIET_TLS_HANDSHAKE_LOGS
#define CONFIG_ORB_WEB_QUIET_TLS_HANDSHAKE_LOGS 1
#endif

#define ORB_WEB_PORTAL_URI_COUNT 7U
#if CONFIG_HTTPD_WS_SUPPORT
#define ORB_WEB_REST_URI_COUNT 18U
#else
#define ORB_WEB_REST_URI_COUNT 17U
#endif
#define ORB_WEB_URI_REQUIRED (ORB_WEB_PORTAL_URI_COUNT + ORB_WEB_REST_URI_COUNT)

typedef struct {
    bool initialized;
    bool started;
    httpd_handle_t server;
    SemaphoreHandle_t state_mutex;
} web_server_ctx_t;

static web_server_ctx_t s_web;
#if CONFIG_ORB_WEB_QUIET_TLS_HANDSHAKE_LOGS
static bool s_tls_log_levels_tuned;
#endif

static TickType_t lock_timeout_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    return ticks > 0 ? ticks : 1;
}

static esp_err_t lock_state(void)
{
    if (s_web.state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_web.state_mutex, lock_timeout_ticks()) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void unlock_state(void)
{
    if (s_web.state_mutex != NULL) {
        xSemaphoreGive(s_web.state_mutex);
    }
}

esp_err_t web_server_init(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "web init denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CONFIG_ORB_ENABLE_WEB) {
        ESP_LOGW(TAG, "web disabled");
        return ESP_OK;
    }
    if (s_web.state_mutex == NULL) {
        s_web.state_mutex = xSemaphoreCreateMutex();
        if (s_web.state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(lock_state(), TAG, "web lock failed");
    if (!s_web.initialized) {
        s_web.initialized = true;
        s_web.started = false;
        s_web.server = NULL;
    }
#if CONFIG_ORB_WEB_QUIET_TLS_HANDSHAKE_LOGS
    if (!s_tls_log_levels_tuned) {
        /* Self-signed HTTPS on LAN can produce frequent client-side abort noise.
         * Keep app logs readable by suppressing low-level TLS spam tags. */
        esp_log_level_set("esp-tls-mbedtls", ESP_LOG_NONE);
        esp_log_level_set("esp_https_server", ESP_LOG_WARN);
        esp_log_level_set("httpd", ESP_LOG_WARN);
        s_tls_log_levels_tuned = true;
    }
#endif
    unlock_state();

    ESP_LOGI(TAG, "web init port=%d", CONFIG_ORB_WEB_PORT);
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "web start denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CONFIG_ORB_ENABLE_WEB) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(web_server_init(), TAG, "web init failed");
    ESP_RETURN_ON_ERROR(lock_state(), TAG, "web lock failed");

    if (s_web.started && s_web.server != NULL) {
        unlock_state();
        return ESP_OK;
    }

    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.server_port = (uint16_t)CONFIG_ORB_WEB_PORT;
    cfg.port_secure = (uint16_t)CONFIG_ORB_WEB_PORT;
    cfg.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
    cfg.httpd.max_open_sockets = 3;
    cfg.httpd.backlog_conn = 2;
    cfg.httpd.max_uri_handlers = (uint16_t)CONFIG_ORB_WEB_MAX_URI_HANDLERS;
    cfg.httpd.stack_size = (uint16_t)CONFIG_ORB_WEB_SERVER_STACK_SIZE;
    cfg.httpd.lru_purge_enable = true;
    cfg.servercert = _binary_servercert_pem_start;
    cfg.servercert_len = (size_t)(_binary_servercert_pem_end - _binary_servercert_pem_start);
    cfg.prvtkey_pem = _binary_prvtkey_pem_start;
    cfg.prvtkey_len = (size_t)(_binary_prvtkey_pem_end - _binary_prvtkey_pem_start);

    if ((unsigned)cfg.httpd.max_uri_handlers < ORB_WEB_URI_REQUIRED) {
        unlock_state();
        ESP_LOGE(TAG,
                 "max_uri_handlers=%u is too small, required>=%u (portal=%u rest=%u)",
                 (unsigned)cfg.httpd.max_uri_handlers,
                 (unsigned)ORB_WEB_URI_REQUIRED,
                 (unsigned)ORB_WEB_PORTAL_URI_COUNT,
                 (unsigned)ORB_WEB_REST_URI_COUNT);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG,
             "starting web server: port=%u max_uri_handlers=%u stack=%u required=%u",
             (unsigned)cfg.httpd.server_port,
             (unsigned)cfg.httpd.max_uri_handlers,
             (unsigned)cfg.httpd.stack_size,
             (unsigned)ORB_WEB_URI_REQUIRED);

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_ssl_start(&server, &cfg);
    if (err != ESP_OK) {
        unlock_state();
        ESP_LOGW(TAG, "httpd_ssl_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rest_api_register_http_handlers(server);
    if (err != ESP_OK) {
        (void)httpd_ssl_stop(server);
        unlock_state();
        ESP_LOGW(TAG, "register rest handlers failed: %s", esp_err_to_name(err));
        return err;
    }

    s_web.server = server;
    s_web.started = true;
    unlock_state();

    ESP_LOGI(TAG,
             "web started port=%d max_uri_handlers=%u stack=%u",
             CONFIG_ORB_WEB_PORT,
             (unsigned)cfg.httpd.max_uri_handlers,
             (unsigned)cfg.httpd.stack_size);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "web stop denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CONFIG_ORB_ENABLE_WEB) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(web_server_init(), TAG, "web init failed");
    ESP_RETURN_ON_ERROR(lock_state(), TAG, "web lock failed");

    if (!s_web.started || s_web.server == NULL) {
        unlock_state();
        return ESP_OK;
    }

    httpd_handle_t server = s_web.server;
    s_web.server = NULL;
    s_web.started = false;
    unlock_state();

    esp_err_t err = httpd_ssl_stop(server);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_ssl_stop failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "web stopped");
    return ESP_OK;
}
