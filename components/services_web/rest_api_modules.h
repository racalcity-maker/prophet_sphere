#ifndef REST_API_MODULES_H
#define REST_API_MODULES_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_portal_register_http_handlers(httpd_handle_t server);

esp_err_t rest_api_register_core_handlers(httpd_handle_t server);
esp_err_t rest_api_register_mode_handlers(httpd_handle_t server);
esp_err_t rest_api_register_led_handlers(httpd_handle_t server);
esp_err_t rest_api_register_audio_handlers(httpd_handle_t server);
esp_err_t rest_api_register_config_handlers(httpd_handle_t server);
esp_err_t rest_api_register_network_handlers(httpd_handle_t server);
esp_err_t rest_api_register_offline_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif
