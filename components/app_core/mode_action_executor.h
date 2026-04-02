#ifndef MODE_ACTION_EXECUTOR_H
#define MODE_ACTION_EXECUTOR_H

#include <stdbool.h>
#include <stdint.h>
#include "app_events.h"
#include "app_mode.h"
#include "esp_err.h"
#include "mode_timers.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool aura_active;
    bool grumble_active;
    bool prophecy_active;
    bool delayed_audio_armed;
    uint32_t grumble_asset_id;
    uint32_t delayed_audio_asset_id;
} mode_action_executor_t;

esp_err_t mode_action_executor_init(mode_action_executor_t *executor);
void mode_action_executor_reset(mode_action_executor_t *executor);
esp_err_t mode_action_executor_prepare_for_mode_switch(mode_action_executor_t *executor, mode_timers_t *timers);
esp_err_t mode_action_executor_before_second_hook(mode_action_executor_t *executor,
                                                  uint32_t second_asset_id,
                                                  uint32_t gap_ms);

bool mode_action_executor_should_suppress_touch_overlay(const mode_action_executor_t *executor, bool session_active);

esp_err_t mode_action_executor_preprocess_event(mode_action_executor_t *executor,
                                                const app_event_t *event,
                                                uint32_t idle_scene_id,
                                                uint32_t grumble_fade_out_ms,
                                                mode_timers_t *timers,
                                                bool *consumed);

esp_err_t mode_action_executor_handle_action(mode_action_executor_t *executor,
                                             const app_mode_action_t *action,
                                             mode_timers_t *timers);

#ifdef __cplusplus
}
#endif

#endif
