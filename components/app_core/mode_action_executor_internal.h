#ifndef MODE_ACTION_EXECUTOR_INTERNAL_H
#define MODE_ACTION_EXECUTOR_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "app_mode.h"
#include "esp_err.h"
#include "mode_action_executor.h"
#include "mode_timers.h"
#include "session_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mode_action_executor_session_state_matches(session_state_t expected);
bool mode_action_executor_session_is_active(void);

esp_err_t mode_action_executor_queue_bg_gain_or_start(uint32_t fade_ms, uint16_t gain_permille);
esp_err_t mode_action_executor_queue_led_scene_and_optional_color(const app_mode_action_t *action);

esp_err_t mode_action_executor_handle_action_start_interaction(mode_action_executor_t *executor,
                                                               const app_mode_action_t *action);
esp_err_t mode_action_executor_handle_action_prophecy_start(mode_action_executor_t *executor,
                                                            const app_mode_action_t *action,
                                                            mode_timers_t *timers);
esp_err_t mode_action_executor_handle_action_play_audio_asset(mode_action_executor_t *executor,
                                                              const app_mode_action_t *action,
                                                              mode_timers_t *timers);
esp_err_t mode_action_executor_handle_action_start_audio_sequence(mode_action_executor_t *executor,
                                                                  const app_mode_action_t *action);
esp_err_t mode_action_executor_handle_action_return_idle(mode_action_executor_t *executor,
                                                         const app_mode_action_t *action,
                                                         mode_timers_t *timers);

esp_err_t mode_action_executor_handle_action_mic_start_capture(const app_mode_action_t *action,
                                                               mode_timers_t *timers);
esp_err_t mode_action_executor_handle_action_mic_stop_capture(mode_timers_t *timers);
esp_err_t mode_action_executor_handle_action_mic_tts_play_text(const app_mode_action_t *action,
                                                               mode_timers_t *timers);
esp_err_t mode_action_executor_handle_action_mic_loopback_start(const app_mode_action_t *action);
esp_err_t mode_action_executor_handle_action_mic_loopback_stop(const app_mode_action_t *action);

#ifdef __cplusplus
}
#endif

#endif
