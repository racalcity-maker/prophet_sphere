#include "app_tasking.h"

#include "app_control_task.h"
#include "app_fsm.h"

esp_err_t app_tasking_start_app_control_task(void)
{
    QueueHandle_t event_queue = app_tasking_get_app_event_queue();
    if (event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (app_fsm_get_state() == APP_FSM_STATE_UNINITIALIZED) {
        esp_err_t err = app_fsm_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    return app_control_task_start(event_queue);
}
