#ifndef LED_TASK_H
#define LED_TASK_H

#include "esp_err.h"

/*
 * Internal LED worker.
 * led_task is the only owner of LED execution/render state.
 */
esp_err_t led_task_start(void);
esp_err_t led_task_stop(void);

#endif
