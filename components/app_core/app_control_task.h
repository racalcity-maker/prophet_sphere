#ifndef APP_CONTROL_TASK_H
#define APP_CONTROL_TASK_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Internal control-task bootstrap.
 * app_control_task owns orchestration and is the only FSM driver.
 */
esp_err_t app_control_task_start(QueueHandle_t app_event_queue);

#endif
