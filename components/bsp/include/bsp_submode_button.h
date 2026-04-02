#ifndef BSP_SUBMODE_BUTTON_H
#define BSP_SUBMODE_BUTTON_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware submode button (active-low, button to GND).
 * Queue-only producer: posts APP_EVENT_SUBMODE_BUTTON_REQUEST.
 * app_control_task/FSM remain the only control-context owners.
 */
esp_err_t bsp_submode_button_start(void);

#ifdef __cplusplus
}
#endif

#endif
