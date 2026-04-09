#include "mode_action_executor_internal.h"

#include "config_manager.h"
#include "control_dispatch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "interaction_sequence.h"
#include "log_tags.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MODE_MANAGER;

esp_err_t mode_action_executor_queue_bg_gain_or_start(uint32_t fade_ms, uint16_t gain_permille)
{
    if (gain_permille > 1000U) {
        gain_permille = 1000U;
    }
    esp_err_t err = control_dispatch_queue_audio_bg_set_gain(fade_ms, gain_permille);
    if (err == ESP_ERR_INVALID_STATE) {
        return control_dispatch_queue_audio_bg_start(fade_ms, gain_permille);
    }
    return err;
}

esp_err_t mode_action_executor_queue_led_scene_and_optional_color(const app_mode_action_t *action)
{
    if (action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (action->led.scene_id != 0U) {
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms),
                            TAG,
                            "LED scene failed");
    }

    if (action->led.color_r != 0U || action->led.color_g != 0U || action->led.color_b != 0U) {
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_aura_color(action->led.color_r,
                                                                  action->led.color_g,
                                                                  action->led.color_b,
                                                                  action->led.fade_ms),
                            TAG,
                            "LED aura color failed");
    }

    return ESP_OK;
}

esp_err_t mode_action_executor_handle_action_prophecy_start(mode_action_executor_t *executor,
                                                            const app_mode_action_t *action,
                                                            mode_timers_t *timers)
{
    if (!mode_action_executor_session_state_matches(SESSION_IDLE)) {
        return ESP_OK;
    }
    if (action->audio.asset_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    executor->prophecy_active = true;
    (void)control_dispatch_queue_led_touch_overlay_clear();

    esp_err_t err = session_controller_start_interaction();
    if (err != ESP_OK) {
        executor->prophecy_active = false;
        return err;
    }

    err = control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms);
    if (err != ESP_OK) {
        (void)session_controller_reset_to_idle();
        executor->prophecy_active = false;
        return err;
    }

    err = control_dispatch_queue_audio_bg_start(action->bg.fade_ms, action->bg.gain_permille);
    if (err != ESP_OK) {
        (void)session_controller_reset_to_idle();
        executor->prophecy_active = false;
        return err;
    }

    if (action->audio.gap_ms > 0U) {
        executor->delayed_audio_asset_id = action->audio.asset_id;
        executor->delayed_audio_armed = true;
        err = mode_timers_start_mode_audio_gap(timers, action->audio.gap_ms);
        if (err != ESP_OK) {
            executor->delayed_audio_asset_id = 0U;
            executor->delayed_audio_armed = false;
            (void)control_dispatch_queue_audio_bg_stop();
            (void)session_controller_reset_to_idle();
            executor->prophecy_active = false;
            return err;
        }
    } else {
        err = control_dispatch_queue_audio_asset(action->audio.asset_id);
        if (err != ESP_OK) {
            (void)control_dispatch_queue_audio_bg_stop();
            (void)session_controller_reset_to_idle();
            executor->prophecy_active = false;
            return err;
        }
    }

    ESP_RETURN_ON_ERROR(session_controller_mark_speaking(), TAG, "session speaking transition failed");
    return ESP_OK;
}

esp_err_t mode_action_executor_handle_action_play_audio_asset(mode_action_executor_t *executor,
                                                              const app_mode_action_t *action,
                                                              mode_timers_t *timers)
{
    if (!mode_action_executor_session_state_matches(SESSION_SPEAKING) &&
        !mode_action_executor_session_state_matches(SESSION_ACTIVATING)) {
        return ESP_OK;
    }
    if (action->audio.asset_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (action->bg.gain_permille > 0U || action->bg.fade_ms > 0U) {
        ESP_RETURN_ON_ERROR(control_dispatch_queue_audio_bg_start(action->bg.fade_ms, action->bg.gain_permille),
                            TAG,
                            "audio bg start before asset failed");
    }
    if (action->led.scene_id != 0U) {
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms), TAG, "LED scene failed");
    }
    if (action->audio.gap_ms > 0U) {
        executor->delayed_audio_asset_id = action->audio.asset_id;
        executor->delayed_audio_armed = true;
        esp_err_t t_err = mode_timers_start_mode_audio_gap(timers, action->audio.gap_ms);
        if (t_err == ESP_OK) {
            return ESP_OK;
        }
        executor->delayed_audio_asset_id = 0U;
        executor->delayed_audio_armed = false;
        ESP_LOGW(TAG, "mode audio gap timer start failed, playing immediately: %s", esp_err_to_name(t_err));
    }
    ESP_RETURN_ON_ERROR(control_dispatch_queue_audio_asset(action->audio.asset_id), TAG, "queue audio asset failed");
    return ESP_OK;
}

esp_err_t mode_action_executor_handle_action_start_audio_sequence(mode_action_executor_t *executor,
                                                                  const app_mode_action_t *action)
{
    if (!mode_action_executor_session_state_matches(SESSION_IDLE)) {
        return ESP_OK;
    }
    uint32_t first_asset_id = action->audio.asset_id;
    uint32_t second_asset_id = action->audio.asset_id_2;
    uint32_t gap_ms = action->audio.gap_ms;

    if (action->audio.sequence_kind == APP_MODE_SEQUENCE_AURA) {
        if (executor->grumble_active) {
            ESP_RETURN_ON_ERROR(control_dispatch_queue_audio_stop(), TAG, "failed to preempt grumble audio");
            executor->grumble_active = false;
            executor->grumble_asset_id = 0U;
            ESP_LOGI(TAG, "aura sequence preempted active grumble");
        }
        executor->prophecy_active = false;
        executor->aura_active = true;
        (void)config_manager_set_aura_selected_color("");
        (void)control_dispatch_queue_led_touch_overlay_clear();

        if (gap_ms == 0U) {
            if (config_manager_get_aura_gap_ms(&gap_ms) != ESP_OK) {
                gap_ms = (uint32_t)CONFIG_ORB_AURA_TRACK_GAP_MS;
            } else {
                if (gap_ms == 0U) {
                    gap_ms = (uint32_t)CONFIG_ORB_AURA_TRACK_GAP_MS;
                }
            }
        }
    } else if (first_asset_id == 0U || second_asset_id == 0U) {
        first_asset_id = (uint32_t)CONFIG_ORB_AURA_TRACK1_ASSET_ID;
        second_asset_id = (uint32_t)CONFIG_ORB_AURA_TRACK2_ASSET_ID;
        if (gap_ms == 0U) {
            gap_ms = (uint32_t)CONFIG_ORB_AURA_TRACK_GAP_MS;
        }
    }

    esp_err_t err = session_controller_start_interaction();
    if (err != ESP_OK) {
        if (action->audio.sequence_kind == APP_MODE_SEQUENCE_AURA) {
            executor->aura_active = false;
            executor->prophecy_active = false;
        }
        return err;
    }
    err = control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms);
    if (err != ESP_OK) {
        (void)session_controller_reset_to_idle();
        if (action->audio.sequence_kind == APP_MODE_SEQUENCE_AURA) {
            executor->aura_active = false;
            executor->prophecy_active = false;
        }
        return err;
    }
    err = interaction_sequence_start_two_track(first_asset_id, second_asset_id, gap_ms);
    if (err != ESP_OK) {
        (void)session_controller_reset_to_idle();
        if (action->audio.sequence_kind == APP_MODE_SEQUENCE_AURA) {
            executor->aura_active = false;
            executor->prophecy_active = false;
        }
        return err;
    }
    ESP_RETURN_ON_ERROR(session_controller_mark_speaking(), TAG, "session speaking transition failed");
    return ESP_OK;
}
