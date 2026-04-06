#include "mode_action_executor_internal.h"

#include "config_manager.h"
#include "control_dispatch.h"
#include "esp_check.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_MODE_MANAGER;

bool mode_action_executor_session_state_matches(session_state_t expected)
{
    session_info_t info = { 0 };
    if (session_controller_get_info(&info) != ESP_OK) {
        return false;
    }
    return info.state == expected;
}

bool mode_action_executor_session_is_active(void)
{
    session_info_t info = { 0 };
    if (session_controller_get_info(&info) != ESP_OK) {
        return false;
    }
    return info.active;
}

esp_err_t mode_action_executor_handle_action_start_interaction(mode_action_executor_t *executor,
                                                               const app_mode_action_t *action)
{
    if (!mode_action_executor_session_state_matches(SESSION_IDLE)) {
        return ESP_OK;
    }
    if (action->audio.sequence_kind == APP_MODE_SEQUENCE_PROPHECY) {
        executor->prophecy_active = true;
        (void)control_dispatch_queue_led_touch_overlay_clear();
    }
    esp_err_t err = session_controller_start_interaction();
    if (err != ESP_OK) {
        if (action->audio.sequence_kind == APP_MODE_SEQUENCE_PROPHECY) {
            executor->prophecy_active = false;
        }
        return err;
    }
    err = control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms);
    if (err != ESP_OK) {
        (void)session_controller_reset_to_idle();
        if (action->audio.sequence_kind == APP_MODE_SEQUENCE_PROPHECY) {
            executor->prophecy_active = false;
        }
        return err;
    }
    err = control_dispatch_queue_audio_asset(action->audio.asset_id);
    if (err != ESP_OK) {
        (void)session_controller_reset_to_idle();
        if (action->audio.sequence_kind == APP_MODE_SEQUENCE_PROPHECY) {
            executor->prophecy_active = false;
        }
        return err;
    }
    ESP_RETURN_ON_ERROR(session_controller_mark_speaking(), TAG, "session speaking transition failed");
    return ESP_OK;
}

esp_err_t mode_action_executor_handle_action_return_idle(mode_action_executor_t *executor,
                                                         const app_mode_action_t *action,
                                                         mode_timers_t *timers)
{
    (void)control_dispatch_queue_audio_stop();
    (void)control_dispatch_queue_audio_pcm_stream_stop();
    (void)control_dispatch_queue_mic_stop_capture();
    (void)control_dispatch_queue_mic_loopback_stop();
    ESP_RETURN_ON_ERROR(control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms), TAG, "LED idle failed");
    if (mode_action_executor_session_state_matches(SESSION_COOLDOWN)) {
        ESP_RETURN_ON_ERROR(session_controller_finish_cooldown(), TAG, "cooldown finish failed");
    } else if (mode_action_executor_session_is_active()) {
        ESP_RETURN_ON_ERROR(session_controller_reset_to_idle(), TAG, "session reset to idle failed");
    }
    executor->aura_active = false;
    executor->prophecy_active = false;
    executor->grumble_active = false;
    executor->grumble_asset_id = 0U;
    executor->delayed_audio_armed = false;
    executor->delayed_audio_asset_id = 0U;
    (void)mode_timers_stop_mode_audio_gap(timers);
    (void)mode_timers_stop_hybrid_ws_timeout(timers);
    (void)config_manager_set_aura_selected_color("");
    return ESP_OK;
}
