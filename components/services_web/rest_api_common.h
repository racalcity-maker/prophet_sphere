#ifndef REST_API_COMMON_H
#define REST_API_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REST_API_QUERY_BUF_LEN 1024U

esp_err_t rest_api_common_init(void);
esp_err_t rest_api_web_buffer_lock(void);
void rest_api_web_buffer_unlock(void);
char *rest_api_shared_json_buffer(size_t *capacity);
void rest_api_set_json_headers(httpd_req_t *req);
esp_err_t rest_api_send_json(httpd_req_t *req, const char *status, const char *json);
esp_err_t rest_api_send_error_json(httpd_req_t *req, const char *status, const char *message);

bool rest_api_parse_u32(const char *text, uint32_t *out);
bool rest_api_parse_bool_text(const char *text, bool *out);
esp_err_t rest_api_query_value(httpd_req_t *req, const char *key, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif
