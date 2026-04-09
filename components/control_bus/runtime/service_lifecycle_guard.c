#include "service_lifecycle_guard.h"

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static portMUX_TYPE s_guard_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_owner_task;
static uint32_t s_owner_depth;

esp_err_t service_lifecycle_guard_enter(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    esp_err_t result = ESP_OK;

    portENTER_CRITICAL(&s_guard_lock);
    if (s_owner_task == NULL || s_owner_task == self) {
        s_owner_task = self;
        s_owner_depth++;
    } else {
        result = ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_guard_lock);

    return result;
}

void service_lifecycle_guard_exit(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    portENTER_CRITICAL(&s_guard_lock);
    if (s_owner_task == self && s_owner_depth > 0U) {
        s_owner_depth--;
        if (s_owner_depth == 0U) {
            s_owner_task = NULL;
        }
    }
    portEXIT_CRITICAL(&s_guard_lock);
}

bool service_lifecycle_guard_is_owned(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    bool owned = false;

    portENTER_CRITICAL(&s_guard_lock);
    owned = (s_owner_task == self && s_owner_depth > 0U);
    portEXIT_CRITICAL(&s_guard_lock);

    return owned;
}
