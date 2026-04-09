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
typedef enum {
    TOUCH_RECONFIG_SCOPE_HOT_APPLY = 0,
    TOUCH_RECONFIG_SCOPE_TASK_RESTART_REQUIRED,
    TOUCH_RECONFIG_SCOPE_SERVICE_RESTART_REQUIRED,
} touch_reconfig_scope_t;

esp_err_t touch_service_init(void);
esp_err_t touch_service_start_task(void);
esp_err_t touch_service_stop_task(void);
bool touch_service_real_touch_enabled(void);
esp_err_t touch_service_get_zone_channels(touch_hw_channel_t channels[TOUCH_ZONE_COUNT]);
esp_err_t touch_service_get_runtime_config(touch_runtime_config_t *out_config);
/*
 * Reconfiguration policy (formalized):
 * - Runtime config fields in touch_runtime_config_t are hot-applied by touch_task.
 * - If recalibrate_now=true, touch_task recalibrates baselines in-place.
 * - Hardware mapping/ownership settings (real-touch enable and zone channels)
 *   are not part of runtime config and require service restart/re-init.
 */
esp_err_t touch_service_apply_runtime_config(const touch_runtime_config_t *config,
                                             bool recalibrate_now,
                                             touch_reconfig_scope_t *out_scope);
esp_err_t touch_service_request_recalibration(void);

#ifdef __cplusplus
}
#endif

#endif
