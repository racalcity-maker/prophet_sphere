#include "rest_api_talk_live_runtime.h"

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

esp_err_t talk_live_runtime_ensure_watchdog(esp_timer_handle_t *watchdog,
                                            esp_timer_cb_t callback,
                                            const char *name,
                                            uint32_t period_ms)
{
    if (watchdog == NULL || callback == NULL || name == NULL || period_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*watchdog != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = callback,
        .arg = NULL,
        .name = name,
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, watchdog);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_timer_start_periodic(*watchdog, (uint64_t)period_ms * 1000ULL);
    if (err != ESP_OK) {
        (void)esp_timer_delete(*watchdog);
        *watchdog = NULL;
        return err;
    }
    return ESP_OK;
}

esp_err_t talk_live_runtime_alloc_ring_samples(int16_t **samples, uint32_t capacity_samples)
{
    if (samples == NULL || capacity_samples == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*samples != NULL) {
        return ESP_OK;
    }

    int16_t *ptr = (int16_t *)heap_caps_malloc((size_t)capacity_samples * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = (int16_t *)heap_caps_malloc((size_t)capacity_samples * sizeof(int16_t),
                                          MALLOC_CAP_8BIT);
    }
    if (ptr == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *samples = ptr;
    return ESP_OK;
}

esp_err_t talk_live_runtime_ensure_feeder_task(TaskHandle_t *task_handle,
                                               bool *stack_psram,
                                               TaskFunction_t entry,
                                               const char *name,
                                               uint32_t stack_size,
                                               UBaseType_t priority)
{
    if (task_handle == NULL || stack_psram == NULL || entry == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = pdFAIL;
    *stack_psram = false;

#if CONFIG_SPIRAM_USE_MALLOC
#if CONFIG_FREERTOS_UNICORE
    ok = xTaskCreateWithCaps(entry,
                             name,
                             stack_size,
                             NULL,
                             priority,
                             task_handle,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    ok = xTaskCreatePinnedToCoreWithCaps(entry,
                                         name,
                                         stack_size,
                                         NULL,
                                         priority,
                                         task_handle,
                                         tskNO_AFFINITY,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (ok == pdPASS && *task_handle != NULL) {
        *stack_psram = true;
        return ESP_OK;
    }
    *task_handle = NULL;
#endif

    *stack_psram = false;
    ok = xTaskCreatePinnedToCore(entry,
                                 name,
                                 stack_size,
                                 NULL,
                                 priority,
                                 task_handle,
                                 tskNO_AFFINITY);
    if (ok != pdPASS || *task_handle == NULL) {
        *task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

