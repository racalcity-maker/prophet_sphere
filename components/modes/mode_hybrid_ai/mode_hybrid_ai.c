#include "mode_hybrid_ai_internal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_MODE_HYBRID;

static hybrid_flow_t s_flow = HYBRID_FLOW_IDLE;
static hybrid_runtime_cfg_t s_runtime = {
    .mic_capture_ms = HYBRID_MIC_CAPTURE_MS_DEFAULT,
    .bg_fade_in_ms = (uint32_t)CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS,
    .bg_fade_out_ms = (uint32_t)CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS,
    .bg_gain_permille = (uint16_t)CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE,
    .unknown_retry_max = (uint8_t)CONFIG_ORB_HYBRID_UNKNOWN_RETRY_MAX,
    .effect_idle_scene_id = (uint32_t)CONFIG_ORB_HYBRID_EFFECT_IDLE_SCENE_ID,
    .effect_talk_scene_id = (uint32_t)CONFIG_ORB_HYBRID_EFFECT_TALK_SCENE_ID,
    .effect_speed = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_SPEED_DEFAULT,
    .effect_intensity = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_INTENSITY_DEFAULT,
    .effect_scale = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_SCALE_DEFAULT,
};
static uint32_t s_expected_fail_asset = 0U;
static uint32_t s_active_capture_id = 0U;
static uint32_t s_next_capture_id = 1U;
static bool s_remote_stream_started = false;
static uint8_t s_unknown_retry_count = 0U;

static uint32_t next_capture_id(void)
{
    uint32_t id = s_next_capture_id++;
    if (s_next_capture_id == 0U) {
        s_next_capture_id = 1U;
    }
    return id;
}

static void reset_flow(void)
{
    s_flow = HYBRID_FLOW_IDLE;
    s_expected_fail_asset = 0U;
    s_active_capture_id = 0U;
    s_remote_stream_started = false;
    s_unknown_retry_count = 0U;
}

static esp_err_t mode_init(void)
{
    hybrid_policy_load_runtime_cfg(&s_runtime);
    reset_flow();
    return ESP_OK;
}

static esp_err_t mode_enter(void)
{
    hybrid_policy_load_runtime_cfg(&s_runtime);
    reset_flow();
    return ESP_OK;
}

static esp_err_t mode_exit(void)
{
    reset_flow();
    return ESP_OK;
}

static esp_err_t mode_handle_event(const app_mode_event_t *event, app_mode_action_t *action)
{
    if (event == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *action = (app_mode_action_t){ 0 };

    ESP_LOGW(TAG,
             "event=%s flow=%s value=%" PRIu32 " code=%" PRId32 " aux=%" PRIu32 " intent=%u conf=%u",
             hybrid_event_name(event->id),
             hybrid_flow_name(s_flow),
             event->value,
             event->code,
             event->aux,
             (unsigned)event->intent_id,
             (unsigned)event->confidence_permille);

    switch (event->id) {
    case APP_MODE_EVENT_TOUCH_DOWN:
        if (s_flow == HYBRID_FLOW_IDLE) {
            action->id = APP_MODE_ACTION_LED_SET_SCENE;
            action->led.scene_id = HYBRID_SCENE_TOUCH_ID;
        }
        break;

    case APP_MODE_EVENT_TOUCH_UP:
        if (s_flow == HYBRID_FLOW_IDLE) {
            action->id = APP_MODE_ACTION_LED_SET_SCENE;
            action->led.scene_id = hybrid_effect_scene_idle(&s_runtime);
        }
        break;

    case APP_MODE_EVENT_TOUCH_HOLD:
        if (s_flow != HYBRID_FLOW_IDLE) {
            break;
        }
        hybrid_policy_load_runtime_cfg(&s_runtime);
        s_active_capture_id = next_capture_id();
        s_expected_fail_asset = HYBRID_FAIL_ASSET_ID;
        action->id = APP_MODE_ACTION_MIC_TTS_PLAY_TEXT;
        action->led.scene_id = hybrid_effect_scene_speak(&s_runtime);
        action->bg.fade_ms = s_runtime.bg_fade_in_ms;
        action->bg.gain_permille = (s_runtime.bg_gain_permille > HYBRID_TTS_BG_GAIN_MAX_PERMILLE)
                                       ? HYBRID_TTS_BG_GAIN_MAX_PERMILLE
                                       : s_runtime.bg_gain_permille;
        action->mic.tts_timeout_ms = HYBRID_REMOTE_TTS_TIMEOUT_MS;
        action->mic.ws_timeout_ms = HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS;
        (void)snprintf(action->mic.tts_text, sizeof(action->mic.tts_text), "%s", HYBRID_REMOTE_INTRO_TTS_TEXT);
        hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE, &s_runtime, action);
        s_remote_stream_started = false;
        ESP_LOGW(TAG, "hybrid start: remote intro capture_id=%" PRIu32, s_active_capture_id);
        break;

    case APP_MODE_EVENT_AUDIO_DONE:
        if (s_flow == HYBRID_FLOW_WAIT_BG_FADE_OUT && hybrid_remote_event_is_bg_fade_done(event)) {
            action->id = APP_MODE_ACTION_RETURN_IDLE;
            action->led.scene_id = hybrid_effect_scene_idle(&s_runtime);
            reset_flow();
            ESP_LOGW(TAG, "bg fade complete -> idle");
            break;
        }

        if (s_flow == HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE && event->value == s_expected_fail_asset) {
            action->id = APP_MODE_ACTION_AUDIO_BG_FADE_OUT;
            action->bg.fade_ms = s_runtime.bg_fade_out_ms;
            hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_BG_FADE_OUT, &s_runtime, action);
            ESP_LOGW(TAG, "local fail prompt complete -> bg fade-out");
            break;
        }
        break;

    case APP_MODE_EVENT_MIC_CAPTURE_DONE:
        if (s_flow != HYBRID_FLOW_WAIT_MIC_DONE || event->value != s_active_capture_id) {
            break;
        }
        if (hybrid_intent_is_unknown_like(event->intent_id) &&
            s_unknown_retry_count < hybrid_unknown_retry_limit(&s_runtime)) {
            s_unknown_retry_count++;
            action->id = APP_MODE_ACTION_MIC_TTS_PLAY_TEXT;
            action->led.scene_id = hybrid_effect_scene_speak(&s_runtime);
            action->led.fade_ms = HYBRID_INTENT_COLOR_RAMP_MS;
            hybrid_resolve_intent_color(event->intent_id, &action->led.color_r, &action->led.color_g, &action->led.color_b);
            action->mic.tts_timeout_ms = HYBRID_REMOTE_TTS_TIMEOUT_MS;
            action->mic.ws_timeout_ms = HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS;
            action->bg.fade_ms = HYBRID_BG_GAIN_SWITCH_FADE_MS;
            action->bg.gain_permille = (s_runtime.bg_gain_permille > HYBRID_TTS_BG_GAIN_MAX_PERMILLE)
                                           ? HYBRID_TTS_BG_GAIN_MAX_PERMILLE
                                           : s_runtime.bg_gain_permille;
            (void)snprintf(action->mic.tts_text, sizeof(action->mic.tts_text), "%s", HYBRID_REMOTE_RETRY_TTS_TEXT);
            hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE, &s_runtime, action);
            s_remote_stream_started = false;
            ESP_LOGW(TAG,
                     "mic done id=%" PRIu32 " unknown intent -> remote retry prompt (%u/%u)",
                     event->value,
                     (unsigned)s_unknown_retry_count,
                     (unsigned)hybrid_unknown_retry_limit(&s_runtime));
            break;
        }
        action->id = APP_MODE_ACTION_MIC_TTS_PLAY_TEXT;
        action->led.scene_id = hybrid_effect_scene_speak(&s_runtime);
        action->led.fade_ms = HYBRID_INTENT_COLOR_RAMP_MS;
        hybrid_resolve_intent_color(event->intent_id, &action->led.color_r, &action->led.color_g, &action->led.color_b);
        action->mic.tts_timeout_ms = HYBRID_REMOTE_TTS_TIMEOUT_MS;
        action->mic.ws_timeout_ms = HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS;
        action->bg.fade_ms = HYBRID_BG_GAIN_SWITCH_FADE_MS;
        action->bg.gain_permille = (s_runtime.bg_gain_permille > HYBRID_TTS_BG_GAIN_MAX_PERMILLE)
                                       ? HYBRID_TTS_BG_GAIN_MAX_PERMILLE
                                       : s_runtime.bg_gain_permille;
        (void)snprintf(action->mic.tts_text, sizeof(action->mic.tts_text), "%s", HYBRID_REMOTE_ANSWER_TTS_TEXT);
        hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE, &s_runtime, action);
        s_remote_stream_started = false;
        ESP_LOGW(TAG,
                 "mic done id=%" PRIu32 " -> remote answer tts (intent=%u conf=%u)",
                 event->value,
                 (unsigned)event->intent_id,
                 (unsigned)event->confidence_permille);
        break;

    case APP_MODE_EVENT_MIC_TTS_DONE:
    case APP_MODE_EVENT_MIC_TTS_ERROR:
        if (s_flow == HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE) {
            if (!s_remote_stream_started || event->id == APP_MODE_EVENT_MIC_TTS_ERROR) {
                action->id = APP_MODE_ACTION_PLAY_AUDIO_ASSET;
                action->audio.asset_id = s_expected_fail_asset;
                hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE, &s_runtime, action);
                ESP_LOGW(TAG, "remote intro failed before completion -> local fail prompt");
                break;
            }
            action->id = APP_MODE_ACTION_MIC_START_CAPTURE;
            action->led.scene_id = hybrid_effect_scene_listen(&s_runtime);
            action->mic.capture_id = s_active_capture_id;
            action->mic.capture_ms = s_runtime.mic_capture_ms;
            action->bg.fade_ms = HYBRID_BG_GAIN_SWITCH_FADE_MS;
            action->bg.gain_permille = HYBRID_BG_GAIN_LISTEN_PERMILLE;
            hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_MIC_DONE, &s_runtime, action);
            ESP_LOGW(TAG,
                     "remote intro done -> bg listen gain + mic capture id=%" PRIu32 " ms=%" PRIu32,
                     s_active_capture_id,
                     s_runtime.mic_capture_ms);
            break;
        }
        if (s_flow == HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE) {
            if (!s_remote_stream_started || event->id == APP_MODE_EVENT_MIC_TTS_ERROR) {
                action->id = APP_MODE_ACTION_PLAY_AUDIO_ASSET;
                action->audio.asset_id = s_expected_fail_asset;
                hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE, &s_runtime, action);
                ESP_LOGW(TAG, "remote retry prompt failed before completion -> local fail prompt");
                break;
            }
            s_active_capture_id = next_capture_id();
            action->id = APP_MODE_ACTION_MIC_START_CAPTURE;
            action->led.scene_id = hybrid_effect_scene_listen(&s_runtime);
            action->mic.capture_id = s_active_capture_id;
            action->mic.capture_ms = s_runtime.mic_capture_ms;
            action->bg.fade_ms = HYBRID_BG_GAIN_SWITCH_FADE_MS;
            action->bg.gain_permille = HYBRID_BG_GAIN_LISTEN_PERMILLE;
            hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_MIC_DONE, &s_runtime, action);
            ESP_LOGW(TAG,
                     "remote retry prompt done -> mic recapture id=%" PRIu32 " ms=%" PRIu32 " (retry=%u/%u)",
                     s_active_capture_id,
                     s_runtime.mic_capture_ms,
                     (unsigned)s_unknown_retry_count,
                     (unsigned)hybrid_unknown_retry_limit(&s_runtime));
            break;
        }
        if (s_flow != HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE) {
            break;
        }
        if (!s_remote_stream_started) {
            action->id = APP_MODE_ACTION_PLAY_AUDIO_ASSET;
            action->audio.asset_id = s_expected_fail_asset;
            hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE, &s_runtime, action);
            ESP_LOGW(TAG, "remote tts finished before first chunk -> local fail prompt");
        } else {
            action->id = APP_MODE_ACTION_AUDIO_BG_FADE_OUT;
            action->bg.fade_ms = s_runtime.bg_fade_out_ms;
            hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_BG_FADE_OUT, &s_runtime, action);
            ESP_LOGW(TAG, "remote tts %s -> bg fade-out", (event->id == APP_MODE_EVENT_MIC_TTS_DONE) ? "done" : "error");
        }
        break;

    case APP_MODE_EVENT_TIMER_EXPIRED:
        if (event->code != HYBRID_WS_TIMEOUT_TIMER_CODE) {
            break;
        }
        if (!hybrid_remote_waiting_tts(s_flow)) {
            break;
        }
        if (s_flow == HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE) {
            action->id = APP_MODE_ACTION_PLAY_AUDIO_ASSET;
            action->audio.asset_id = s_expected_fail_asset;
            hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE, &s_runtime, action);
            ESP_LOGW(TAG,
                     "remote intro timeout %" PRIu32 "ms -> local fail prompt",
                     (uint32_t)HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS);
        } else if (!s_remote_stream_started) {
            action->id = APP_MODE_ACTION_PLAY_AUDIO_ASSET;
            action->audio.asset_id = s_expected_fail_asset;
            hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE, &s_runtime, action);
            ESP_LOGW(TAG,
                     "remote answer first chunk timeout %" PRIu32 "ms -> local fail prompt",
                     (uint32_t)HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS);
        } else {
            action->id = APP_MODE_ACTION_AUDIO_BG_FADE_OUT;
            action->bg.fade_ms = s_runtime.bg_fade_out_ms;
            hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_BG_FADE_OUT, &s_runtime, action);
            ESP_LOGW(TAG,
                     "remote answer stream max timeout %" PRIu32 "ms -> bg fade-out",
                     (uint32_t)HYBRID_REMOTE_STREAM_MAX_MS);
        }
        break;

    case APP_MODE_EVENT_MIC_TTS_STREAM_STARTED:
    {
        bool waiting_remote_tts = hybrid_remote_waiting_tts(s_flow);
        bool capture_matches = hybrid_remote_capture_matches(event->value, s_active_capture_id);
        if (!waiting_remote_tts || s_remote_stream_started) {
            break;
        }
        if (s_flow == HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE && !capture_matches) {
            break;
        }
        if ((s_flow == HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE ||
             s_flow == HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE) &&
            !capture_matches) {
            ESP_LOGW(TAG,
                     "remote stream started with non-matching capture id=%" PRIu32
                     " (expected=%" PRIu32 ", flow=%s) -> accept",
                     event->value,
                     s_active_capture_id,
                     hybrid_flow_name(s_flow));
        }
        s_remote_stream_started = true;
        action->id = APP_MODE_ACTION_HYBRID_WS_TIMER_START;
        action->mic.ws_timeout_ms = hybrid_remote_stream_timeout_ms(s_flow);
        ESP_LOGW(TAG,
                 "remote first chunk received -> switch timeout to stream max %" PRIu32 "ms (flow=%s)",
                 action->mic.ws_timeout_ms,
                 hybrid_flow_name(s_flow));
        break;
    }

    case APP_MODE_EVENT_MIC_REMOTE_PLAN_ERROR:
        break;

    case APP_MODE_EVENT_AUDIO_ERROR:
    case APP_MODE_EVENT_MIC_ERROR:
        if (s_flow != HYBRID_FLOW_IDLE) {
            if (s_flow == HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE ||
                s_flow == HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE ||
                s_flow == HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE ||
                s_flow == HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE) {
                action->id = APP_MODE_ACTION_AUDIO_BG_FADE_OUT;
                action->bg.fade_ms = s_runtime.bg_fade_out_ms;
                hybrid_set_flow_and_scene(&s_flow, HYBRID_FLOW_WAIT_BG_FADE_OUT, &s_runtime, action);
                ESP_LOGW(TAG, "flow error -> bg fade-out");
            } else {
                action->id = APP_MODE_ACTION_RETURN_IDLE;
                action->led.scene_id = hybrid_effect_scene_idle(&s_runtime);
                reset_flow();
                ESP_LOGW(TAG, "flow error -> idle");
            }
        }
        break;

    default:
        break;
    }

    return ESP_OK;
}

const app_mode_t *mode_hybrid_ai_get(void)
{
    static const app_mode_t mode = {
        .id = ORB_MODE_HYBRID_AI,
        .name = "hybrid_ai",
        .init = mode_init,
        .enter = mode_enter,
        .exit = mode_exit,
        .handle_event = mode_handle_event,
    };
    return &mode;
}

