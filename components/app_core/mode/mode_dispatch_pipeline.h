#ifndef MODE_DISPATCH_PIPELINE_H
#define MODE_DISPATCH_PIPELINE_H

#include <stdint.h>
#include "app_defs.h"
#include "app_events.h"
#include "app_mode.h"
#include "esp_err.h"
#include "mode_action_executor.h"
#include "mode_timers.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mode_action_executor_t *action_executor;
    mode_timers_t *mode_timers;
    uint32_t offline_grumble_fade_out_ms;
} mode_dispatch_pipeline_ctx_t;

esp_err_t mode_dispatch_pipeline_run(mode_dispatch_pipeline_ctx_t *ctx,
                                     orb_mode_t current_mode,
                                     const app_mode_t *mode,
                                     const app_event_t *event);

#ifdef __cplusplus
}
#endif

#endif

