#ifndef ORB_MODE_RUNTIME_POLICY_H
#define ORB_MODE_RUNTIME_POLICY_H

#include "app_defs.h"
#include "esp_err.h"

esp_err_t orb_mode_runtime_apply(orb_mode_t previous_mode, orb_mode_t target_mode);

#endif
