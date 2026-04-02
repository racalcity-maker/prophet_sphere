#ifndef APP_FSM_H
#define APP_FSM_H

#include "esp_err.h"
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_FSM_STATE_UNINITIALIZED = 0,
    APP_FSM_STATE_BOOTING,
    APP_FSM_STATE_RUNNING,
    APP_FSM_STATE_MODE_TRANSITION,
    APP_FSM_STATE_DEGRADED,
    APP_FSM_STATE_ERROR,
} app_fsm_state_t;

/*
 * app_fsm_* APIs are control-context APIs and are intended to run from
 * app_control_task.
 */
esp_err_t app_fsm_init(void);
esp_err_t app_fsm_handle_event(const app_event_t *event);
app_fsm_state_t app_fsm_get_state(void);
const char *app_fsm_state_to_str(app_fsm_state_t state);

#ifdef __cplusplus
}
#endif

#endif
