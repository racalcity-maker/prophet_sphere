#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_service_init(void);
esp_err_t mqtt_service_start(void);
esp_err_t mqtt_service_stop(void);

#ifdef __cplusplus
}
#endif

#endif
