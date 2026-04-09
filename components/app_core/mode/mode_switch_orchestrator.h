#ifndef MODE_SWITCH_ORCHESTRATOR_H
#define MODE_SWITCH_ORCHESTRATOR_H

#include "app_defs.h"
#include "app_mode.h"
#include "esp_err.h"
#include "mode_action_executor.h"
#include "mode_manager.h"
#include "mode_timers.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mode_action_executor_t *action_executor;
    mode_timers_t *mode_timers;
    mode_runtime_apply_hook_t runtime_hook;
} mode_switch_orchestrator_ctx_t;

esp_err_t mode_switch_orchestrator_run(mode_switch_orchestrator_ctx_t *ctx,
                                       orb_mode_t previous_mode,
                                       orb_mode_t target_mode,
                                       const app_mode_t *previous_desc,
                                       const app_mode_t *target_desc);

#ifdef __cplusplus
}
#endif

#endif

