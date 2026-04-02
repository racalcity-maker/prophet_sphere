#ifndef BSP_MODE_BUTTON_H
#define BSP_MODE_BUTTON_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware mode button (active-low, button to GND).
 * This module never performs mode switch directly.
 * It only posts queue-safe mode switch requests via mode_manager API.
 */
esp_err_t bsp_mode_button_start(void);

#ifdef __cplusplus
}
#endif

#endif

