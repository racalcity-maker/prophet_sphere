#ifndef MODE_SWITCH_CLEANUP_H
#define MODE_SWITCH_CLEANUP_H

#include "esp_err.h"
#include "mode_action_executor.h"
#include "mode_timers.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mode_switch_cleanup_run(mode_action_executor_t *executor, mode_timers_t *timers);

#ifdef __cplusplus
}
#endif

#endif

