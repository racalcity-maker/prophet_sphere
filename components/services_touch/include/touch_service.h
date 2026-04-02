#ifndef TOUCH_SERVICE_H
#define TOUCH_SERVICE_H

#include <stdbool.h>
#include "esp_err.h"
#include "touch_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * touch_task is the only producer of normalized APP_EVENT_TOUCH_* events.
 * touch_service owns touch peripheral setup and zone channel mapping.
 */
esp_err_t touch_service_init(void);
esp_err_t touch_service_start_task(void);
esp_err_t touch_service_stop_task(void);
bool touch_service_real_touch_enabled(void);
esp_err_t touch_service_get_zone_channels(touch_hw_channel_t channels[TOUCH_ZONE_COUNT]);

#ifdef __cplusplus
}
#endif

#endif
