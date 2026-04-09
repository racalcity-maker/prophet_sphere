#ifndef TOUCH_TASK_H
#define TOUCH_TASK_H

#include "esp_err.h"
#include "touch_types.h"

/*
 * Internal touch worker.
 * touch_task is the only producer of normalized touch events.
 */
esp_err_t touch_task_start(void);
esp_err_t touch_task_stop(void);
void touch_task_load_default_runtime_config(touch_runtime_config_t *out_config);
esp_err_t touch_task_get_runtime_config(touch_runtime_config_t *out_config);
esp_err_t touch_task_apply_runtime_config(const touch_runtime_config_t *config, bool recalibrate_now);

#endif
