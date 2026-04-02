#ifndef SUBMODE_CONTROLLER_H
#define SUBMODE_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>
#include "app_defs.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t submode_controller_init(void);
uint32_t submode_controller_idle_scene_for_mode(orb_mode_t mode);
bool submode_controller_is_offline_lottery_active(orb_mode_t mode);
esp_err_t submode_controller_handle_request(orb_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
