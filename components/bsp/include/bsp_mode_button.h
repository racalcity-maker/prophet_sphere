#ifndef BSP_MODE_BUTTON_H
#define BSP_MODE_BUTTON_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bsp_mode_button_pressed_cb_t)(void *user_ctx);

/*
 * Hardware mode button (active-low, button to GND).
 * BSP does not depend on app/mode orchestration.
 * App layer should register callback and decide what to do on press.
 */
esp_err_t bsp_mode_button_set_pressed_callback(bsp_mode_button_pressed_cb_t callback, void *user_ctx);
esp_err_t bsp_mode_button_start(void);

#ifdef __cplusplus
}
#endif

#endif
