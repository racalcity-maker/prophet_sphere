#include "mode_action_executor_internal.h"

#include "control_dispatch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_MODE_MANAGER;

esp_err_t mode_action_executor_handle_action_mic_start_capture(const app_mode_action_t *action,
                                                               mode_timers_t *timers)
{
    if (action->mic.capture_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(mode_action_executor_queue_led_scene_and_optional_color(action),
                        TAG,
                        "LED update before mic capture failed");
    if (action->bg.gain_permille > 0U || action->bg.fade_ms > 0U) {
        ESP_RETURN_ON_ERROR(mode_action_executor_queue_bg_gain_or_start(action->bg.fade_ms, action->bg.gain_permille),
                            TAG,
                            "audio bg set gain before mic capture failed");
    }
    ESP_RETURN_ON_ERROR(control_dispatch_queue_mic_start_capture(action->mic.capture_id, action->mic.capture_ms),
                        TAG,
                        "mic capture start failed");
    return ESP_OK;
}

esp_err_t mode_action_executor_handle_action_mic_stop_capture(mode_timers_t *timers)
{
    (void)mode_timers_stop_hybrid_ws_timeout(timers);
    ESP_RETURN_ON_ERROR(control_dispatch_queue_mic_stop_capture(), TAG, "mic capture stop failed");
    return ESP_OK;
}

esp_err_t mode_action_executor_handle_action_mic_tts_play_text(const app_mode_action_t *action,
                                                               mode_timers_t *timers)
{
    if (action->mic.tts_text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    bool session_started_here = false;
    bool ws_timer_started = false;
    bool pcm_started = false;

    if (mode_action_executor_session_state_matches(SESSION_IDLE)) {
        ESP_RETURN_ON_ERROR(session_controller_start_interaction(), TAG, "session start for remote tts failed");
        session_started_here = true;
        esp_err_t led_err = mode_action_executor_queue_led_scene_and_optional_color(action);
        if (led_err != ESP_OK) {
            (void)session_controller_reset_to_idle();
            return led_err;
        }
        ESP_RETURN_ON_ERROR(session_controller_mark_speaking(), TAG, "session speaking transition failed");
    } else {
        ESP_RETURN_ON_ERROR(mode_action_executor_queue_led_scene_and_optional_color(action),
                            TAG,
                            "LED update before mic tts failed");
    }
    if (action->mic.ws_timeout_ms > 0U) {
        esp_err_t timer_err = mode_timers_start_hybrid_ws_timeout(timers, action->mic.ws_timeout_ms);
        if (timer_err != ESP_OK) {
            if (session_started_here) {
                (void)session_controller_reset_to_idle();
            }
            ESP_LOGW(TAG, "hybrid ws timeout timer start failed: %s", esp_err_to_name(timer_err));
            return timer_err;
        }
        ws_timer_started = true;
    }
    if (action->bg.gain_permille > 0U || action->bg.fade_ms > 0U) {
        esp_err_t bg_err = mode_action_executor_queue_bg_gain_or_start(action->bg.fade_ms, action->bg.gain_permille);
        if (bg_err != ESP_OK) {
            if (ws_timer_started) {
                (void)mode_timers_stop_hybrid_ws_timeout(timers);
            }
            if (session_started_here) {
                (void)session_controller_reset_to_idle();
            }
            ESP_LOGW(TAG, "audio bg set gain before mic tts failed: %s", esp_err_to_name(bg_err));
            return bg_err;
        }
    }

    esp_err_t pcm_err = control_dispatch_queue_audio_pcm_stream_start();
    if (pcm_err != ESP_OK) {
        if (ws_timer_started) {
            (void)mode_timers_stop_hybrid_ws_timeout(timers);
        }
        if (session_started_here) {
            (void)session_controller_reset_to_idle();
        }
        ESP_LOGW(TAG, "audio pcm stream start failed: %s", esp_err_to_name(pcm_err));
        return pcm_err;
    }
    pcm_started = true;

    esp_err_t mic_err = control_dispatch_queue_mic_tts_play_text(action->mic.tts_text, action->mic.tts_timeout_ms);
    if (mic_err != ESP_OK) {
        if (pcm_started) {
            esp_err_t rollback_pcm_err = control_dispatch_queue_audio_pcm_stream_stop();
            if (rollback_pcm_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "mic tts play failed (%s), and pcm rollback failed: %s",
                         esp_err_to_name(mic_err),
                         esp_err_to_name(rollback_pcm_err));
            } else {
                ESP_LOGW(TAG, "mic tts play failed (%s), rolled back pcm stream", esp_err_to_name(mic_err));
            }
        }
        if (ws_timer_started) {
            (void)mode_timers_stop_hybrid_ws_timeout(timers);
        }
        if (session_started_here) {
            (void)session_controller_reset_to_idle();
        }
        return mic_err;
    }

    return ESP_OK;
}

esp_err_t mode_action_executor_handle_action_mic_loopback_start(const app_mode_action_t *action)
{
    if (action->led.scene_id != 0U) {
        (void)control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms);
    }
    esp_err_t pre_audio_stop_err = control_dispatch_queue_audio_stop();
    if (pre_audio_stop_err != ESP_OK) {
        ESP_LOGW(TAG, "audio stop before loopback returned %s", esp_err_to_name(pre_audio_stop_err));
    }
    esp_err_t pre_mic_stop_err = control_dispatch_queue_mic_stop_capture();
    if (pre_mic_stop_err != ESP_OK) {
        ESP_LOGW(TAG, "mic capture stop before loopback returned %s", esp_err_to_name(pre_mic_stop_err));
    }
    esp_err_t pcm_err = control_dispatch_queue_audio_pcm_stream_start();
    if (pcm_err != ESP_OK) {
        ESP_LOGW(TAG, "audio pcm stream start before loopback failed: %s", esp_err_to_name(pcm_err));
        return pcm_err;
    }

    esp_err_t mic_err = control_dispatch_queue_mic_loopback_start();
    if (mic_err != ESP_OK) {
        esp_err_t rollback_pcm_err = control_dispatch_queue_audio_pcm_stream_stop();
        if (rollback_pcm_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "mic loopback start failed (%s), and pcm rollback failed: %s",
                     esp_err_to_name(mic_err),
                     esp_err_to_name(rollback_pcm_err));
        } else {
            ESP_LOGW(TAG, "mic loopback start failed (%s), rolled back pcm stream", esp_err_to_name(mic_err));
        }
        return mic_err;
    }

    return ESP_OK;
}

esp_err_t mode_action_executor_handle_action_mic_loopback_stop(const app_mode_action_t *action)
{
    if (action->led.scene_id != 0U) {
        (void)control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms);
    }
    (void)control_dispatch_queue_mic_loopback_stop();
    (void)control_dispatch_queue_audio_pcm_stream_stop();
    return ESP_OK;
}
