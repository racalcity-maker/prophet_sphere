#ifndef AI_TASK_H
#define AI_TASK_H

#include "esp_err.h"

/*
 * Internal AI worker.
 * ai_task is the only owner of AI execution state.
 */
esp_err_t ai_task_start(void);
esp_err_t ai_task_stop(void);

#endif
