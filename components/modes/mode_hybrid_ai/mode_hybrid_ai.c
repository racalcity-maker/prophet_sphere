#include "app_mode.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "config_manager.h"
#include "esp_log.h"
#include "log_tags.h"
#include "orb_led_scenes.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MODE_HYBRID;

#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS
#define CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS 2000
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS
#define CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS 4000
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE
#define CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE 260
#endif

#define HYBRID_SCENE_IDLE_ID ORB_LED_SCENE_ID_HYBRID_IDLE_SLOW_BREATHE
#define HYBRID_SCENE_TOUCH_ID ORB_LED_SCENE_ID_HYBRID_TOUCH_FAST_BREATHE
#define HYBRID_SCENE_VORTEX_ID ORB_LED_SCENE_ID_HYBRID_VORTEX
#define HYBRID_SCENE_VORTEX_LISTEN_ID ORB_LED_SCENE_ID_HYBRID_VORTEX_DIM
#define HYBRID_FAIL_ASSET_ID 1303U

#define HYBRID_MIC_CAPTURE_MS_DEFAULT 8000U
#define HYBRID_REMOTE_TTS_TIMEOUT_MS 90000U
#define HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS 10000U
#define HYBRID_REMOTE_STREAM_MAX_MS 90000U
#define HYBRID_REMOTE_INTRO_STREAM_MAX_MS 30000U
#define HYBRID_WS_TIMEOUT_TIMER_CODE 6
#define HYBRID_TTS_STREAM_STARTED_MARKER UINT16_MAX
#define HYBRID_REMOTE_INTRO_TTS_TEXT "__ORACLE_INTRO__"
#define HYBRID_REMOTE_ANSWER_TTS_TEXT "__ORACLE_AUTO__"
#define HYBRID_REMOTE_RETRY_TTS_TEXT "__ORACLE_RETRY__"
/* Keep TTS background lower than prophecy background. */
#define HYBRID_TTS_BG_GAIN_MAX_PERMILLE 180U
#define HYBRID_BG_GAIN_LISTEN_PERMILLE 100U
#define HYBRID_BG_GAIN_SWITCH_FADE_MS 250U
#define HYBRID_INTENT_COLOR_RAMP_MS 800U

#ifndef CONFIG_ORB_HYBRID_UNKNOWN_RETRY_MAX
#define CONFIG_ORB_HYBRID_UNKNOWN_RETRY_MAX 1
#endif

typedef enum {
    HYBRID_FLOW_IDLE = 0,
    HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE,
    HYBRID_FLOW_WAIT_MIC_DONE,
    HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE,
    HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE,
    HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE,
    HYBRID_FLOW_WAIT_BG_FADE_OUT,
} hybrid_flow_t;

typedef struct {
    uint32_t mic_capture_ms;
    uint32_t bg_fade_in_ms;
    uint32_t bg_fade_out_ms;
    uint16_t bg_gain_permille;
    uint8_t unknown_retry_max;
} hybrid_runtime_cfg_t;

static hybrid_flow_t s_flow = HYBRID_FLOW_IDLE;
static hybrid_runtime_cfg_t s_runtime = {
    .mic_capture_ms = HYBRID_MIC_CAPTURE_MS_DEFAULT,
    .bg_fade_in_ms = (uint32_t)CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS,
    .bg_fade_out_ms = (uint32_t)CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS,
    .bg_gain_permille = (uint16_t)CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE,
    .unknown_retry_max = (uint8_t)CONFIG_ORB_HYBRID_UNKNOWN_RETRY_MAX,
};
static uint32_t s_expected_fail_asset = 0U;
static uint32_t s_active_capture_id = 0U;
static uint32_t s_next_capture_id = 1U;
static bool s_remote_stream_started = false;
static uint8_t s_unknown_retry_count = 0U;

static uint8_t unknown_retry_limit(void)
{
    uint32_t v = (uint32_t)s_runtime.unknown_retry_max;
    if (v > 2U) {
        v = 2U;
    }
    return (uint8_t)v;
}

static bool intent_is_unknown_like(uint8_t intent_id)
{
    return intent_id == ORB_INTENT_UNKNOWN || intent_id == ORB_INTENT_UNCERTAIN;
}

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

static void load_runtime_cfg(void)
{
    orb_runtime_config_t runtime = { 0 };
    if (config_manager_get_snapshot(&runtime) != ESP_OK) {
        return;
    }

    s_runtime.bg_fade_in_ms = runtime.prophecy_bg_fade_in_ms;
    s_runtime.bg_fade_out_ms = runtime.prophecy_bg_fade_out_ms;
    s_runtime.bg_gain_permille = runtime.prophecy_bg_gain_permille;
    s_runtime.unknown_retry_max = (runtime.hybrid_unknown_retry_max <= 2U)
                                      ? runtime.hybrid_unknown_retry_max
                                      : 2U;
    (void)runtime.hybrid_mic_capture_ms;
    /* Hybrid remote flow uses fixed 8s listen window. */
    s_runtime.mic_capture_ms = HYBRID_MIC_CAPTURE_MS_DEFAULT;
}

static bool event_is_bg_fade_done(const app_mode_event_t *event)
{
    return event != NULL &&
           event->id == APP_MODE_EVENT_AUDIO_DONE &&
           event->code == (int32_t)APP_MODE_AUDIO_DONE_CODE_BG_FADE_COMPLETE;
}

static uint32_t scene_for_flow(hybrid_flow_t flow)
{
    switch (flow) {
    case HYBRID_FLOW_WAIT_MIC_DONE:
        return HYBRID_SCENE_VORTEX_LISTEN_ID;
    case HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE:
    case HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE:
    case HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE:
    case HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE:
    case HYBRID_FLOW_WAIT_BG_FADE_OUT:
        return HYBRID_SCENE_VORTEX_ID;
    case HYBRID_FLOW_IDLE:
    default:
        return HYBRID_SCENE_IDLE_ID;
    }
}

static void set_flow_and_scene(hybrid_flow_t next_flow, app_mode_action_t *action)
{
    s_flow = next_flow;
    if (action != NULL) {
        action->led.scene_id = scene_for_flow(next_flow);
    }
}

static void resolve_intent_color(uint8_t intent_id, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r == NULL || g == NULL || b == NULL) {
        return;
    }

    switch (intent_id) {
    case ORB_INTENT_LOVE:
        *r = 255U;
        *g = 70U;
        *b = 170U; /* pink */
        break;
    case ORB_INTENT_DANGER:
        *r = 255U;
        *g = 25U;
        *b = 20U; /* red */
        break;
    case ORB_INTENT_PATH:
        *r = 255U;
        *g = 215U;
        *b = 40U; /* yellow */
        break;
    case ORB_INTENT_MONEY:
        *r = 40U;
        *g = 255U;
        *b = 90U; /* green */
        break;
    case ORB_INTENT_CHOICE:
        *r = 70U;
        *g = 220U;
        *b = 255U; /* cyan */
        break;
    case ORB_INTENT_FUTURE:
        *r = 145U;
        *g = 100U;
        *b = 255U; /* violet */
        break;
    case ORB_INTENT_INNER_STATE:
        *r = 70U;
        *g = 130U;
        *b = 255U; /* blue */
        break;
    case ORB_INTENT_WISH:
        *r = 220U;
        *g = 80U;
        *b = 255U; /* magenta */
        break;
    case ORB_INTENT_TIME:
        *r = 255U;
        *g = 155U;
        *b = 40U; /* orange */
        break;
    case ORB_INTENT_PAST:
        *r = 70U;
        *g = 180U;
        *b = 180U; /* teal */
        break;
    case ORB_INTENT_PLACE:
        *r = 255U;
        *g = 180U;
        *b = 80U; /* warm amber */
        break;
    case ORB_INTENT_FORBIDDEN:
        *r = 255U;
        *g = 32U;
        *b = 32U; /* hard red */
        break;
    case ORB_INTENT_JOKE:
        *r = 255U;
        *g = 255U;
        *b = 90U; /* playful yellow */
        break;
    case ORB_INTENT_UNCERTAIN:
    case ORB_INTENT_UNKNOWN:
    default:
        *r = 170U;
        *g = 200U;
        *b = 255U; /* neutral cold */
        break;
    }
}

static const char *flow_name(hybrid_flow_t flow)
{
    switch (flow) {
    case HYBRID_FLOW_IDLE:
        return "idle";
    case HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE:
        return "wait_remote_intro_tts_done";
    case HYBRID_FLOW_WAIT_MIC_DONE:
        return "wait_mic_done";
    case HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE:
        return "wait_remote_retry_tts_done";
    case HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE:
        return "wait_remote_answer_tts_done";
    case HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE:
        return "wait_fail_prompt_done";
    case HYBRID_FLOW_WAIT_BG_FADE_OUT:
        return "wait_bg_fade";
    default:
        return "unknown";
    }
}

static const char *event_name(app_mode_event_id_t id)
{
    switch (id) {
    case APP_MODE_EVENT_TOUCH_HOLD:
        return "touch_hold";
    case APP_MODE_EVENT_TOUCH_DOWN:
        return "touch_down";
    case APP_MODE_EVENT_TOUCH_UP:
        return "touch_up";
    case APP_MODE_EVENT_AUDIO_DONE:
        return "audio_done";
    case APP_MODE_EVENT_AUDIO_ERROR:
        return "audio_error";
    case APP_MODE_EVENT_MIC_CAPTURE_DONE:
        return "mic_done";
    case APP_MODE_EVENT_MIC_ERROR:
        return "mic_error";
    case APP_MODE_EVENT_MIC_REMOTE_PLAN_READY:
        return "mic_plan_ready";
    case APP_MODE_EVENT_MIC_REMOTE_PLAN_ERROR:
        return "mic_plan_error";
    case APP_MODE_EVENT_MIC_TTS_DONE:
        return "mic_tts_done";
    case APP_MODE_EVENT_MIC_TTS_ERROR:
        return "mic_tts_error";
    case APP_MODE_EVENT_TIMER_EXPIRED:
        return "timer_expired";
    default:
        return "other";
    }
}

static esp_err_t mode_init(void)
{
    reset_flow();
    return ESP_OK;
}

static esp_err_t mode_enter(void)
{
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
             event_name(event->id),
             flow_name(s_flow),
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
            action->led.scene_id = HYBRID_SCENE_IDLE_ID;
        }
        break;

    case APP_MODE_EVENT_TOUCH_HOLD:
        if (s_flow != HYBRID_FLOW_IDLE) {
            break;
        }
        load_runtime_cfg();
        s_active_capture_id = next_capture_id();
        s_expected_fail_asset = HYBRID_FAIL_ASSET_ID;
        action->id = APP_MODE_ACTION_MIC_TTS_PLAY_TEXT;
        action->led.scene_id = HYBRID_SCENE_VORTEX_ID;
        action->bg.fade_ms = s_runtime.bg_fade_in_ms;
        action->bg.gain_permille = (s_runtime.bg_gain_permille > HYBRID_TTS_BG_GAIN_MAX_PERMILLE)
                                       ? HYBRID_TTS_BG_GAIN_MAX_PERMILLE
                                       : s_runtime.bg_gain_permille;
        action->mic.tts_timeout_ms = HYBRID_REMOTE_TTS_TIMEOUT_MS;
        action->mic.ws_timeout_ms = HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS;
        (void)snprintf(action->mic.tts_text, sizeof(action->mic.tts_text), "%s", HYBRID_REMOTE_INTRO_TTS_TEXT);
        set_flow_and_scene(HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE, action);
        s_remote_stream_started = false;
        ESP_LOGW(TAG,
                 "hybrid start: remote intro capture_id=%" PRIu32,
                 s_active_capture_id);
        break;

    case APP_MODE_EVENT_AUDIO_DONE:
        if (s_flow == HYBRID_FLOW_WAIT_BG_FADE_OUT && event_is_bg_fade_done(event)) {
            action->id = APP_MODE_ACTION_RETURN_IDLE;
            action->led.scene_id = HYBRID_SCENE_IDLE_ID;
            reset_flow();
            ESP_LOGW(TAG, "bg fade complete -> idle");
            break;
        }

        if (s_flow == HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE && event->value == s_expected_fail_asset) {
            action->id = APP_MODE_ACTION_AUDIO_BG_FADE_OUT;
            action->bg.fade_ms = s_runtime.bg_fade_out_ms;
            set_flow_and_scene(HYBRID_FLOW_WAIT_BG_FADE_OUT, action);
            ESP_LOGW(TAG, "local fail prompt complete -> bg fade-out");
            break;
        }
        break;

    case APP_MODE_EVENT_MIC_CAPTURE_DONE:
        if (s_flow != HYBRID_FLOW_WAIT_MIC_DONE || event->value != s_active_capture_id) {
            break;
        }
        if (intent_is_unknown_like(event->intent_id) && s_unknown_retry_count < unknown_retry_limit()) {
            s_unknown_retry_count++;
            action->id = APP_MODE_ACTION_MIC_TTS_PLAY_TEXT;
            action->led.scene_id = HYBRID_SCENE_VORTEX_ID;
            action->led.fade_ms = HYBRID_INTENT_COLOR_RAMP_MS;
            resolve_intent_color(event->intent_id, &action->led.color_r, &action->led.color_g, &action->led.color_b);
            action->mic.tts_timeout_ms = HYBRID_REMOTE_TTS_TIMEOUT_MS;
            action->mic.ws_timeout_ms = HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS;
            action->bg.fade_ms = HYBRID_BG_GAIN_SWITCH_FADE_MS;
            action->bg.gain_permille = (s_runtime.bg_gain_permille > HYBRID_TTS_BG_GAIN_MAX_PERMILLE)
                                           ? HYBRID_TTS_BG_GAIN_MAX_PERMILLE
                                           : s_runtime.bg_gain_permille;
            (void)snprintf(action->mic.tts_text, sizeof(action->mic.tts_text), "%s", HYBRID_REMOTE_RETRY_TTS_TEXT);
            set_flow_and_scene(HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE, action);
            s_remote_stream_started = false;
            ESP_LOGW(TAG,
                     "mic done id=%" PRIu32 " unknown intent -> remote retry prompt (%u/%u)",
                     event->value,
                     (unsigned)s_unknown_retry_count,
                     (unsigned)unknown_retry_limit());
            break;
        }
        action->id = APP_MODE_ACTION_MIC_TTS_PLAY_TEXT;
        action->led.scene_id = HYBRID_SCENE_VORTEX_ID;
        action->led.fade_ms = HYBRID_INTENT_COLOR_RAMP_MS;
        resolve_intent_color(event->intent_id, &action->led.color_r, &action->led.color_g, &action->led.color_b);
        action->mic.tts_timeout_ms = HYBRID_REMOTE_TTS_TIMEOUT_MS;
        action->mic.ws_timeout_ms = HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS;
        action->bg.fade_ms = HYBRID_BG_GAIN_SWITCH_FADE_MS;
        action->bg.gain_permille = (s_runtime.bg_gain_permille > HYBRID_TTS_BG_GAIN_MAX_PERMILLE)
                                       ? HYBRID_TTS_BG_GAIN_MAX_PERMILLE
                                       : s_runtime.bg_gain_permille;
        (void)snprintf(action->mic.tts_text, sizeof(action->mic.tts_text), "%s", HYBRID_REMOTE_ANSWER_TTS_TEXT);
        set_flow_and_scene(HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE, action);
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
                set_flow_and_scene(HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE, action);
                ESP_LOGW(TAG, "remote intro failed before completion -> local fail prompt");
                break;
            }
            action->id = APP_MODE_ACTION_MIC_START_CAPTURE;
            action->led.scene_id = HYBRID_SCENE_VORTEX_LISTEN_ID;
            action->mic.capture_id = s_active_capture_id;
            action->mic.capture_ms = s_runtime.mic_capture_ms;
            action->bg.fade_ms = HYBRID_BG_GAIN_SWITCH_FADE_MS;
            action->bg.gain_permille = HYBRID_BG_GAIN_LISTEN_PERMILLE;
            set_flow_and_scene(HYBRID_FLOW_WAIT_MIC_DONE, action);
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
                set_flow_and_scene(HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE, action);
                ESP_LOGW(TAG, "remote retry prompt failed before completion -> local fail prompt");
                break;
            }
            s_active_capture_id = next_capture_id();
            action->id = APP_MODE_ACTION_MIC_START_CAPTURE;
            action->led.scene_id = HYBRID_SCENE_VORTEX_LISTEN_ID;
            action->mic.capture_id = s_active_capture_id;
            action->mic.capture_ms = s_runtime.mic_capture_ms;
            action->bg.fade_ms = HYBRID_BG_GAIN_SWITCH_FADE_MS;
            action->bg.gain_permille = HYBRID_BG_GAIN_LISTEN_PERMILLE;
            set_flow_and_scene(HYBRID_FLOW_WAIT_MIC_DONE, action);
            ESP_LOGW(TAG,
                     "remote retry prompt done -> mic recapture id=%" PRIu32 " ms=%" PRIu32 " (retry=%u/%u)",
                     s_active_capture_id,
                     s_runtime.mic_capture_ms,
                     (unsigned)s_unknown_retry_count,
                     (unsigned)unknown_retry_limit());
            break;
        }
        if (s_flow != HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE) {
            break;
        }
        if (!s_remote_stream_started) {
            action->id = APP_MODE_ACTION_PLAY_AUDIO_ASSET;
            action->audio.asset_id = s_expected_fail_asset;
            set_flow_and_scene(HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE, action);
            ESP_LOGW(TAG, "remote tts finished before first chunk -> local fail prompt");
        } else {
            action->id = APP_MODE_ACTION_AUDIO_BG_FADE_OUT;
            action->bg.fade_ms = s_runtime.bg_fade_out_ms;
            set_flow_and_scene(HYBRID_FLOW_WAIT_BG_FADE_OUT, action);
            ESP_LOGW(TAG,
                     "remote tts %s -> bg fade-out",
                     (event->id == APP_MODE_EVENT_MIC_TTS_DONE) ? "done" : "error");
        }
        break;

    case APP_MODE_EVENT_TIMER_EXPIRED:
        if (event->code != HYBRID_WS_TIMEOUT_TIMER_CODE) {
            break;
        }
        if (s_flow != HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE &&
            s_flow != HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE &&
            s_flow != HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE) {
            break;
        }
        if (s_flow == HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE) {
            action->id = APP_MODE_ACTION_PLAY_AUDIO_ASSET;
            action->audio.asset_id = s_expected_fail_asset;
            set_flow_and_scene(HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE, action);
            ESP_LOGW(TAG,
                     "remote intro timeout %" PRIu32 "ms -> local fail prompt",
                     (uint32_t)HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS);
        } else if (!s_remote_stream_started) {
            action->id = APP_MODE_ACTION_PLAY_AUDIO_ASSET;
            action->audio.asset_id = s_expected_fail_asset;
            set_flow_and_scene(HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE, action);
            ESP_LOGW(TAG,
                     "remote answer first chunk timeout %" PRIu32 "ms -> local fail prompt",
                     (uint32_t)HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS);
        } else {
            action->id = APP_MODE_ACTION_AUDIO_BG_FADE_OUT;
            action->bg.fade_ms = s_runtime.bg_fade_out_ms;
            set_flow_and_scene(HYBRID_FLOW_WAIT_BG_FADE_OUT, action);
            ESP_LOGW(TAG,
                     "remote answer stream max timeout %" PRIu32 "ms -> bg fade-out",
                     (uint32_t)HYBRID_REMOTE_STREAM_MAX_MS);
        }
        break;

    case APP_MODE_EVENT_MIC_REMOTE_PLAN_READY:
        if ((s_flow != HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE &&
             s_flow != HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE &&
             s_flow != HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE) ||
            s_remote_stream_started ||
            event->code != (int32_t)HYBRID_TTS_STREAM_STARTED_MARKER) {
            break;
        }
        s_remote_stream_started = true;
        action->id = APP_MODE_ACTION_HYBRID_WS_TIMER_START;
        action->mic.ws_timeout_ms = (s_flow == HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE ||
                                     s_flow == HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE)
                                        ? HYBRID_REMOTE_INTRO_STREAM_MAX_MS
                                        : HYBRID_REMOTE_STREAM_MAX_MS;
        ESP_LOGW(TAG,
                 "remote first chunk received -> switch timeout to stream max %" PRIu32 "ms (flow=%s)",
                 action->mic.ws_timeout_ms,
                 flow_name(s_flow));
        break;

    case APP_MODE_EVENT_MIC_REMOTE_PLAN_ERROR:
        /* Informational only in remote-only flow. */
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
                set_flow_and_scene(HYBRID_FLOW_WAIT_BG_FADE_OUT, action);
                ESP_LOGW(TAG, "flow error -> bg fade-out");
            } else {
                action->id = APP_MODE_ACTION_RETURN_IDLE;
                action->led.scene_id = HYBRID_SCENE_IDLE_ID;
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

