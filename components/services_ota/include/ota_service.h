#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OTA service lifecycle API.
 * Network/Web callbacks should only call request APIs; heavy OTA work
 * belongs to dedicated service context in future iterations.
 */
esp_err_t ota_service_init(void);
esp_err_t ota_service_start(void);
esp_err_t ota_service_stop(void);
esp_err_t ota_service_mark_boot_success(void);
esp_err_t ota_service_request_update(const char *url);
bool ota_service_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
