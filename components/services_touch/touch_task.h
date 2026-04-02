#ifndef TOUCH_TASK_H
#define TOUCH_TASK_H

#include "esp_err.h"

/*
 * Internal touch worker.
 * touch_task is the only producer of normalized touch events.
 */
esp_err_t touch_task_start(void);
esp_err_t touch_task_stop(void);

#endif
