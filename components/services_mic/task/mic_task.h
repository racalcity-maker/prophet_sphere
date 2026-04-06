#ifndef MIC_TASK_H
#define MIC_TASK_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "app_tasking.h"
#include "mic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mic_task_start(QueueHandle_t command_queue);
esp_err_t mic_task_stop(void);
esp_err_t mic_task_get_status(mic_capture_status_t *out_status);

#ifdef __cplusplus
}
#endif

#endif
