#ifndef REST_API_TALK_LIVE_RUNTIME_H
#define REST_API_TALK_LIVE_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t talk_live_runtime_ensure_watchdog(esp_timer_handle_t *watchdog,
                                            esp_timer_cb_t callback,
                                            const char *name,
                                            uint32_t period_ms);

esp_err_t talk_live_runtime_alloc_ring_samples(int16_t **samples, uint32_t capacity_samples);

esp_err_t talk_live_runtime_ensure_feeder_task(TaskHandle_t *task_handle,
                                               bool *stack_psram,
                                               TaskFunction_t entry,
                                               const char *name,
                                               uint32_t stack_size,
                                               UBaseType_t priority);

#endif

