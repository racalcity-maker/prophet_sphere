#include "mode_action_executor.h"
#include "mode_action_executor_internal.h"

#include <inttypes.h>
#include <string.h>
#include "config_manager.h"
#include "control_dispatch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "interaction_sequence.h"
#include "log_tags.h"
#include "orb_led_scenes.h"
#include "sdkconfig.h"
#include "session_controller.h"

static const char *TAG = LOG_TAG_MODE_MANAGER;
static const uint32_t MODE_SWITCH_QUICK_QUEUE_TIMEOUT_MS = 5U;

typedef struct {
    const char *name;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} aura_color_entry_t;

static const aura_color_entry_t s_aura_colors[] = {
    { "red", 255U, 40U, 20U },
    { "orange", 255U, 110U, 20U },
    { "yellow", 255U, 220U, 40U },
    { "lime", 180U, 255U, 20U },
    { "green", 40U, 255U, 60U },
    { "turquoise", 20U, 255U, 180U },
    { "cyan", 20U, 220U, 255U },
    { "sky", 80U, 170U, 255U },
    { "blue", 30U, 90U, 255U },
    { "indigo", 90U, 70U, 255U },
    { "violet", 150U, 70U, 255U },
    { "pink", 255U, 80U, 170U },
};

static const aura_color_entry_t *select_random_aura_color(void)
{
    size_t count = sizeof(s_aura_colors) / sizeof(s_aura_colors[0]);
    if (count == 0U) {
        return NULL;
    }
    uint32_t idx = esp_random() % (uint32_t)count;
    return &s_aura_colors[idx];
}

static void queue_audio_stop_quick(void)
{
    (void)MODE_SWITCH_QUICK_QUEUE_TIMEOUT_MS;
    esp_err_t err = control_dispatch_queue_audio_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "quick queue audio stop failed: %s", esp_err_to_name(err));
    }
}

static void queue_audio_pcm_stop_quick(void)
{
    (void)MODE_SWITCH_QUICK_QUEUE_TIMEOUT_MS;
    esp_err_t err = control_dispatch_queue_audio_pcm_stream_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "quick queue audio pcm stop failed: %s", esp_err_to_name(err));
    }
}

static void queue_mic_stop_capture_quick(void)
{
    (void)MODE_SWITCH_QUICK_QUEUE_TIMEOUT_MS;
    esp_err_t err = control_dispatch_queue_mic_stop_capture();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "quick queue mic stop capture failed: %s", esp_err_to_name(err));
    }
}

static void queue_mic_loopback_stop_quick(void)
{
    (void)MODE_SWITCH_QUICK_QUEUE_TIMEOUT_MS;
    esp_err_t err = control_dispatch_queue_mic_loopback_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "quick queue mic loopback stop failed: %s", esp_err_to_name(err));
    }
}

esp_err_t mode_action_executor_init(mode_action_executor_t *executor)
{
    if (executor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(executor, 0, sizeof(*executor));
    (void)config_manager_set_aura_selected_color("");
    return ESP_OK;
}

void mode_action_executor_reset(mode_action_executor_t *executor)
{
    if (executor == NULL) {
        return;
    }
    executor->aura_active = false;
    executor->grumble_active = false;
    executor->prophecy_active = false;
    executor->delayed_audio_armed = false;
    executor->grumble_asset_id = 0U;
    executor->delayed_audio_asset_id = 0U;
    (void)config_manager_set_aura_selected_color("");
}

esp_err_t mode_action_executor_prepare_for_mode_switch(mode_action_executor_t *executor, mode_timers_t *timers)
{
    if (executor == NULL || timers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    (void)control_dispatch_queue_led_stop();
    queue_audio_stop_quick();
    queue_audio_pcm_stop_quick();
    queue_mic_stop_capture_quick();
    queue_mic_loopback_stop_quick();
    (void)mode_timers_stop_all(timers);

    (void)session_controller_reset_to_idle();
    (void)interaction_sequence_reset();
    mode_action_executor_reset(executor);

    orb_runtime_config_t cfg = { 0 };
    uint8_t brightness = (config_manager_get_snapshot(&cfg) == ESP_OK) ? cfg.led_brightness : CONFIG_ORB_LED_DEFAULT_BRIGHTNESS;
    (void)control_dispatch_queue_led_brightness(brightness);
    return ESP_OK;
}

esp_err_t mode_action_executor_before_second_hook(mode_action_executor_t *executor,
                                                  uint32_t second_asset_id,
                                                  uint32_t gap_ms)
{
    (void)second_asset_id;
    if (executor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!executor->aura_active) {
        return ESP_OK;
    }

    const aura_color_entry_t *color = select_random_aura_color();
    if (color == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(config_manager_set_aura_selected_color(color->name), TAG, "set aura color failed");
    ESP_RETURN_ON_ERROR(control_dispatch_queue_led_scene(ORB_LED_SCENE_ID_AURA_COLOR_BREATHE, 0U), TAG, "set aura scene failed");
    ESP_RETURN_ON_ERROR(control_dispatch_queue_led_aura_color(color->r, color->g, color->b, gap_ms), TAG, "set aura color failed");

    ESP_LOGI(TAG, "aura color selected: %s (ramp=%" PRIu32 "ms)", color->name, gap_ms);
    return ESP_OK;
}

bool mode_action_executor_should_suppress_touch_overlay(const mode_action_executor_t *executor, bool session_active)
{
    if (executor == NULL) {
        return false;
    }
    return (executor->aura_active || executor->prophecy_active) && session_active;
}

esp_err_t mode_action_executor_preprocess_event(mode_action_executor_t *executor,
                                                const app_event_t *event,
                                                uint32_t idle_scene_id,
                                                uint32_t grumble_fade_out_ms,
                                                mode_timers_t *timers,
                                                bool *consumed)
{
    if (executor == NULL || event == NULL || timers == NULL || consumed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *consumed = false;

    if (event->id == APP_EVENT_AUDIO_LEVEL) {
        uint32_t raw_level = event->payload.scalar.value;
        uint8_t level = (raw_level > 255U) ? 255U : (uint8_t)raw_level;
        if (control_dispatch_queue_led_audio_level(level) != ESP_OK) {
            ESP_LOGW(TAG, "failed to queue audio reactive level=%u", (unsigned)level);
        }
        *consumed = true;
        return ESP_OK;
    }

    if (event->id == APP_EVENT_TIMER_EXPIRED &&
        event->payload.scalar.code == (int32_t)APP_TIMER_KIND_GRUMBLE_FADE) {
        (void)control_dispatch_queue_led_scene(idle_scene_id, 0U);
        orb_runtime_config_t cfg = { 0 };
        uint8_t brightness = (config_manager_get_snapshot(&cfg) == ESP_OK) ? cfg.led_brightness : CONFIG_ORB_LED_DEFAULT_BRIGHTNESS;
        (void)control_dispatch_queue_led_brightness(brightness);
        *consumed = true;
        return ESP_OK;
    }

    if (event->id == APP_EVENT_TIMER_EXPIRED &&
        event->payload.scalar.code == (int32_t)APP_TIMER_KIND_MODE_AUDIO_GAP) {
        if (executor->delayed_audio_armed && executor->delayed_audio_asset_id != 0U) {
            uint32_t asset_id = executor->delayed_audio_asset_id;
            executor->delayed_audio_armed = false;
            executor->delayed_audio_asset_id = 0U;
            if (control_dispatch_queue_audio_asset(asset_id) != ESP_OK) {
                ESP_LOGW(TAG, "delayed audio play failed id=%" PRIu32, asset_id);
            }
        }
        *consumed = true;
        return ESP_OK;
    }

    if (event->id == APP_EVENT_MIC_CAPTURE_DONE ||
        event->id == APP_EVENT_MIC_ERROR ||
        event->id == APP_EVENT_MIC_REMOTE_PLAN_READY ||
        event->id == APP_EVENT_MIC_TTS_STREAM_STARTED ||
        event->id == APP_EVENT_MIC_TTS_DONE ||
        event->id == APP_EVENT_MIC_TTS_ERROR) {
        (void)mode_timers_stop_hybrid_ws_timeout(timers);
    }

    if (event->id == APP_EVENT_AUDIO_DONE &&
        executor->grumble_active &&
        event->payload.scalar.value == executor->grumble_asset_id) {
        ESP_LOGI(TAG, "grumble complete asset=%" PRIu32, executor->grumble_asset_id);
        executor->grumble_active = false;
        executor->grumble_asset_id = 0U;
        (void)control_dispatch_queue_led_aura_fade_out(grumble_fade_out_ms);
        (void)mode_timers_start_grumble_fade(timers, grumble_fade_out_ms);
        return ESP_OK;
    }

    if (event->id == APP_EVENT_AUDIO_ERROR &&
        executor->grumble_active &&
        event->payload.scalar.value == executor->grumble_asset_id) {
        ESP_LOGW(TAG,
                 "grumble error asset=%" PRIu32 " code=%" PRId32,
                 executor->grumble_asset_id,
                 event->payload.scalar.code);
        executor->grumble_active = false;
        executor->grumble_asset_id = 0U;
        (void)control_dispatch_queue_led_aura_fade_out(grumble_fade_out_ms);
        (void)mode_timers_start_grumble_fade(timers, grumble_fade_out_ms);
        return ESP_OK;
    }

    return ESP_OK;
}

esp_err_t mode_action_executor_handle_action(mode_action_executor_t *executor,
                                             const app_mode_action_t *action,
                                             mode_timers_t *timers)
{
    if (executor == NULL || action == NULL || timers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (action->id == APP_MODE_ACTION_NONE) {
        return ESP_OK;
    }

    switch (action->id) {
    case APP_MODE_ACTION_START_INTERACTION: {
        return mode_action_executor_handle_action_start_interaction(executor, action);
    }
    case APP_MODE_ACTION_PROPHECY_START: {
        return mode_action_executor_handle_action_prophecy_start(executor, action, timers);
    }
    case APP_MODE_ACTION_PLAY_AUDIO_ASSET:
        return mode_action_executor_handle_action_play_audio_asset(executor, action, timers);
    case APP_MODE_ACTION_PLAY_GRUMBLE:
        if (action->audio.asset_id == 0U) {
            return ESP_ERR_INVALID_ARG;
        }
        if (executor->aura_active || mode_action_executor_session_is_active() || executor->grumble_active) {
            return ESP_OK;
        }
        {
            uint32_t grumble_scene = (action->led.scene_id != 0U) ? action->led.scene_id : ORB_LED_SCENE_ID_GRUMBLE_RED;
            ESP_RETURN_ON_ERROR(control_dispatch_queue_led_scene(grumble_scene, 0U), TAG, "failed to set grumble scene");
        }
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_aura_color(255U, 0U, 0U, 0U), TAG, "failed to set grumble red color");
        ESP_RETURN_ON_ERROR(control_dispatch_queue_audio_asset(action->audio.asset_id), TAG, "queue grumble audio failed");
        executor->grumble_active = true;
        executor->grumble_asset_id = action->audio.asset_id;
        ESP_LOGI(TAG, "grumble started asset=%" PRIu32, action->audio.asset_id);
        return ESP_OK;
    case APP_MODE_ACTION_BEGIN_COOLDOWN:
        if (!mode_action_executor_session_state_matches(SESSION_SPEAKING) && !mode_action_executor_session_state_matches(SESSION_COOLDOWN)) {
            return ESP_OK;
        }
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms), TAG, "LED cooldown failed");
        ESP_RETURN_ON_ERROR(session_controller_begin_cooldown(
                                (action->timing.cooldown_ms > 0U) ? action->timing.cooldown_ms : (uint32_t)CONFIG_ORB_OFFLINE_COOLDOWN_MS),
                            TAG,
                            "cooldown start failed");
        return ESP_OK;
    case APP_MODE_ACTION_START_AUDIO_SEQUENCE: {
        return mode_action_executor_handle_action_start_audio_sequence(executor, action);
    }
    case APP_MODE_ACTION_AURA_FADE_OUT:
        if (!mode_action_executor_session_state_matches(SESSION_COOLDOWN)) {
            return ESP_OK;
        }
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_aura_fade_out((action->led.fade_ms > 0U) ? action->led.fade_ms : 1000U),
                            TAG,
                            "LED aura fade-out failed");
        ESP_RETURN_ON_ERROR(session_controller_begin_cooldown(
                                (action->timing.cooldown_ms > 0U) ? action->timing.cooldown_ms : 1000U),
                            TAG,
                            "aura fade cooldown start failed");
        return ESP_OK;
    case APP_MODE_ACTION_AUDIO_BG_START:
        if (action->mic.ws_timeout_ms > 0U) {
            ESP_RETURN_ON_ERROR(mode_timers_start_hybrid_ws_timeout(timers, action->mic.ws_timeout_ms),
                                TAG,
                                "hybrid ws timeout timer start failed");
        }
        ESP_RETURN_ON_ERROR(control_dispatch_queue_audio_bg_start(action->bg.fade_ms, action->bg.gain_permille),
                            TAG,
                            "audio bg start failed");
        return ESP_OK;
    case APP_MODE_ACTION_AUDIO_BG_SET_GAIN:
        ESP_RETURN_ON_ERROR(mode_action_executor_queue_bg_gain_or_start(action->bg.fade_ms, action->bg.gain_permille),
                            TAG,
                            "audio bg set gain failed");
        return ESP_OK;
    case APP_MODE_ACTION_AUDIO_BG_FADE_OUT:
        (void)mode_timers_stop_hybrid_ws_timeout(timers);
        ESP_RETURN_ON_ERROR(control_dispatch_queue_audio_bg_fade_out(action->bg.fade_ms), TAG, "audio bg fade failed");
        return ESP_OK;
    case APP_MODE_ACTION_AUDIO_BG_STOP:
        (void)mode_timers_stop_hybrid_ws_timeout(timers);
        ESP_RETURN_ON_ERROR(control_dispatch_queue_audio_bg_stop(), TAG, "audio bg stop failed");
        return ESP_OK;
    case APP_MODE_ACTION_HYBRID_WS_TIMER_START:
        if (action->mic.ws_timeout_ms == 0U) {
            return ESP_ERR_INVALID_ARG;
        }
        ESP_RETURN_ON_ERROR(mode_timers_start_hybrid_ws_timeout(timers, action->mic.ws_timeout_ms),
                            TAG,
                            "hybrid ws timeout timer start failed");
        return ESP_OK;
    case APP_MODE_ACTION_MIC_START_CAPTURE:
        return mode_action_executor_handle_action_mic_start_capture(action, timers);
    case APP_MODE_ACTION_MIC_STOP_CAPTURE:
        return mode_action_executor_handle_action_mic_stop_capture(timers);
    case APP_MODE_ACTION_MIC_TTS_PLAY_TEXT:
        return mode_action_executor_handle_action_mic_tts_play_text(action, timers);
    case APP_MODE_ACTION_MIC_LOOPBACK_START:
        return mode_action_executor_handle_action_mic_loopback_start(action);
    case APP_MODE_ACTION_MIC_LOOPBACK_STOP:
        return mode_action_executor_handle_action_mic_loopback_stop(action);
    case APP_MODE_ACTION_RETURN_IDLE:
        return mode_action_executor_handle_action_return_idle(executor, action, timers);
    case APP_MODE_ACTION_LED_SET_SCENE:
        return mode_action_executor_queue_led_scene_and_optional_color(action);
    case APP_MODE_ACTION_LOTTERY_START_SORTING:
        (void)control_dispatch_queue_led_touch_overlay_clear();
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms),
                            TAG,
                            "lottery sorting scene failed");
        ESP_RETURN_ON_ERROR(control_dispatch_queue_audio_asset(action->audio.asset_id), TAG, "lottery sorting audio failed");
        return ESP_OK;
    case APP_MODE_ACTION_LOTTERY_ASSIGN_TEAM:
        (void)control_dispatch_queue_led_touch_overlay_clear();
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms),
                            TAG,
                            "lottery assign scene failed");
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_aura_color(action->led.color_r, action->led.color_g, action->led.color_b, 0U),
                            TAG,
                            "lottery assign color failed");
        ESP_RETURN_ON_ERROR(control_dispatch_queue_audio_asset(action->audio.asset_id), TAG, "lottery team audio failed");
        return ESP_OK;
    case APP_MODE_ACTION_LOTTERY_ABORT:
        (void)control_dispatch_queue_led_touch_overlay_clear();
        ESP_RETURN_ON_ERROR(control_dispatch_queue_audio_stop(), TAG, "lottery abort audio stop failed");
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms),
                            TAG,
                            "lottery abort scene failed");
        return ESP_OK;
    case APP_MODE_ACTION_LOTTERY_RETURN_IDLE:
        (void)control_dispatch_queue_led_touch_overlay_clear();
        ESP_RETURN_ON_ERROR(control_dispatch_queue_led_scene(action->led.scene_id, action->led.duration_ms),
                            TAG,
                            "lottery return idle scene failed");
        return ESP_OK;
    case APP_MODE_ACTION_NONE:
    default:
        return ESP_OK;
    }
}
