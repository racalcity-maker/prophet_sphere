#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_portal_register_http_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif

