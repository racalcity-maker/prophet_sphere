#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_init(void);
esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif
