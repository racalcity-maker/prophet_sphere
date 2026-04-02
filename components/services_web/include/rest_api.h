#ifndef REST_API_H
#define REST_API_H

#include "app_defs.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rest_api_init(void);
esp_err_t rest_api_register_http_handlers(httpd_handle_t server);
esp_err_t rest_api_request_mode_switch(orb_mode_t target_mode);

#ifdef __cplusplus
}
#endif

#endif
