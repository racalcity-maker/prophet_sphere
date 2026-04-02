#include "offline_submode.h"

#include <stdbool.h>
#include <stdint.h>
#include "config_manager.h"
#include "esp_log.h"
#include "log_tags.h"
#include "orb_led_scenes.h"
#include "prophecy_common.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MODE_OFFLINE;

#ifndef CONFIG_ORB_OFFLINE_PROPHECY_SCENE_IDLE_ID
#define CONFIG_ORB_OFFLINE_PROPHECY_SCENE_IDLE_ID ORB_LED_SCENE_ID_FIRE2012
#endif
#ifndef CONFIG_ORB_OFFLINE_PROPHECY_SCENE_ACTIVE_ID
#define CONFIG_ORB_OFFLINE_PROPHECY_SCENE_ACTIVE_ID ORB_LED_SCENE_ID_PLASMA
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS
#define CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS 2000
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS
#define CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS 4000
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE
#define CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE 260
#endif

#define PROPHECY_GAP12_DEFAULT_MS 800U
#define PROPHECY_GAP23_DEFAULT_MS 800U
#define PROPHECY_GAP34_DEFAULT_MS 2000U
#define PROPHECY_LEADIN_DEFAULT_MS 1000U
#define PROPHECY_BG_FADE_IN_DEFAULT_MS ((uint32_t)CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS)
#define PROPHECY_BG_FADE_OUT_DEFAULT_MS ((uint32_t)CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS)
#define PROPHECY_BG_GAIN_DEFAULT_PERMILLE ((uint16_t)CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE)

typedef enum {
    PROPHECY_FLOW_IDLE = 0,
    PROPHECY_FLOW_PLAYING_GREET,
    PROPHECY_FLOW_PLAYING_UNDERSTANDING,
    PROPHECY_FLOW_PLAYING_PREDICTION,
    PROPHECY_FLOW_PLAYING_FAREWELL,
    PROPHECY_FLOW_WAIT_BG_FADE_OUT,
} prophecy_flow_t;

typedef struct {
    uint32_t gap12_ms;
    uint32_t gap23_ms;
    uint32_t gap34_ms;
    uint32_t leadin_wait_ms;
    uint32_t bg_fade_in_ms;
    uint32_t bg_fade_out_ms;
    uint16_t bg_gain_permille;
} prophecy_runtime_cfg_t;

static prophecy_flow_t s_flow = PROPHECY_FLOW_IDLE;
static prophecy_archetype_t s_archetype = PROPHECY_ARCHETYPE_CHOICE;
static prophecy_phase_t s_phase = PROPHECY_PHASE_GREET;
static uint32_t s_expected_asset = 0U;
static prophecy_runtime_cfg_t s_runtime = {
    .gap12_ms = PROPHECY_GAP12_DEFAULT_MS,
    .gap23_ms = PROPHECY_GAP23_DEFAULT_MS,
    .gap34_ms = PROPHECY_GAP34_DEFAULT_MS,
    .leadin_wait_ms = PROPHECY_LEADIN_DEFAULT_MS,
    .bg_fade_in_ms = PROPHECY_BG_FADE_IN_DEFAULT_MS,
    .bg_fade_out_ms = PROPHECY_BG_FADE_OUT_DEFAULT_MS,
    .bg_gain_permille = PROPHECY_BG_GAIN_DEFAULT_PERMILLE,
};

static bool is_bg_fade_done_event(const app_mode_event_t *event)
{
    if (event == NULL || event->id != APP_MODE_EVENT_AUDIO_DONE) {
        return false;
    }
    return event->code == (int32_t)APP_MODE_AUDIO_DONE_CODE_BG_FADE_COMPLETE;
}

static void load_runtime_cfg(prophecy_runtime_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    orb_runtime_config_t runtime = { 0 };
    if (config_manager_get_snapshot(&runtime) != ESP_OK) {
        return;
    }
    cfg->gap12_ms = runtime.prophecy_gap12_ms;
    cfg->gap23_ms = runtime.prophecy_gap23_ms;
    cfg->gap34_ms = runtime.prophecy_gap34_ms;
    cfg->leadin_wait_ms = runtime.prophecy_leadin_wait_ms;
    cfg->bg_fade_in_ms = runtime.prophecy_bg_fade_in_ms;
    cfg->bg_fade_out_ms = runtime.prophecy_bg_fade_out_ms;
    cfg->bg_gain_permille = runtime.prophecy_bg_gain_permille;
}

static void reset_flow(void)
{
    s_flow = PROPHECY_FLOW_IDLE;
    s_phase = PROPHECY_PHASE_GREET;
    s_expected_asset = 0U;
}

static bool queue_next_phase_action(app_mode_action_t *action, prophecy_flow_t next_flow, uint32_t gap_ms)
{
    if (action == NULL) {
        return false;
    }

    prophecy_phase_t next_phase = PROPHECY_PHASE_GREET;
    uint32_t next_asset = 0U;
    if (!prophecy_phase_advance(s_archetype, s_phase, &next_phase, &next_asset)) {
        return false;
    }

    s_phase = next_phase;
    s_expected_asset = next_asset;
    action->id = APP_MODE_ACTION_PLAY_AUDIO_ASSET;
    action->audio.asset_id = next_asset;
    action->audio.gap_ms = gap_ms;
    s_flow = next_flow;

    ESP_LOGI(TAG, "offline.prophecy phase=%s archetype=%s", prophecy_phase_name(s_phase), prophecy_archetype_name(s_archetype));
    return true;
}

static esp_err_t prophecy_init(void)
{
    reset_flow();
    ESP_LOGI(TAG, "offline.prophecy init");
    return ESP_OK;
}

static esp_err_t prophecy_enter(void)
{
    reset_flow();
    ESP_LOGI(TAG, "offline.prophecy enter");
    return ESP_OK;
}

static esp_err_t prophecy_exit(void)
{
    reset_flow();
    ESP_LOGI(TAG, "offline.prophecy exit");
    return ESP_OK;
}

static esp_err_t prophecy_handle_event(const app_mode_event_t *event, app_mode_action_t *action)
{
    if (event == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *action = (app_mode_action_t){ 0 };

    switch (event->id) {
    case APP_MODE_EVENT_TOUCH_HOLD:
        if (s_flow != PROPHECY_FLOW_IDLE) {
            break;
        }
        load_runtime_cfg(&s_runtime);
        s_archetype = prophecy_random_archetype();
        s_phase = PROPHECY_PHASE_GREET;
        s_expected_asset = prophecy_asset_for(s_archetype, s_phase);
        if (s_expected_asset == 0U) {
            reset_flow();
            return ESP_ERR_INVALID_STATE;
        }

        action->id = APP_MODE_ACTION_PROPHECY_START;
        action->audio.sequence_kind = APP_MODE_SEQUENCE_PROPHECY;
        action->led.scene_id = (uint32_t)CONFIG_ORB_OFFLINE_PROPHECY_SCENE_ACTIVE_ID;
        action->led.duration_ms = 0U;
        action->audio.asset_id = s_expected_asset;
        action->audio.gap_ms = s_runtime.bg_fade_in_ms + s_runtime.leadin_wait_ms;
        action->bg.fade_ms = s_runtime.bg_fade_in_ms;
        action->bg.gain_permille = s_runtime.bg_gain_permille;
        s_flow = PROPHECY_FLOW_PLAYING_GREET;

        ESP_LOGI(TAG, "offline.prophecy start archetype=%s", prophecy_archetype_name(s_archetype));
        break;

    case APP_MODE_EVENT_AUDIO_DONE:
        if (s_flow == PROPHECY_FLOW_WAIT_BG_FADE_OUT && is_bg_fade_done_event(event)) {
            action->id = APP_MODE_ACTION_RETURN_IDLE;
            action->led.scene_id = (uint32_t)CONFIG_ORB_OFFLINE_PROPHECY_SCENE_IDLE_ID;
            s_flow = PROPHECY_FLOW_IDLE;
            s_phase = PROPHECY_PHASE_GREET;
            s_expected_asset = 0U;
            ESP_LOGI(TAG, "offline.prophecy bg fade complete -> idle");
            break;
        }
        if (s_flow == PROPHECY_FLOW_IDLE || s_flow == PROPHECY_FLOW_WAIT_BG_FADE_OUT) {
            break;
        }
        if (event->value != s_expected_asset) {
            break;
        }

        if (s_flow == PROPHECY_FLOW_PLAYING_GREET) {
            if (!queue_next_phase_action(action, PROPHECY_FLOW_PLAYING_UNDERSTANDING, s_runtime.gap12_ms)) {
                action->id = APP_MODE_ACTION_RETURN_IDLE;
                action->led.scene_id = (uint32_t)CONFIG_ORB_OFFLINE_PROPHECY_SCENE_IDLE_ID;
                reset_flow();
                ESP_LOGW(TAG, "offline.prophecy failed to queue next phase from greeting");
            }
            break;
        }
        if (s_flow == PROPHECY_FLOW_PLAYING_UNDERSTANDING) {
            if (!queue_next_phase_action(action, PROPHECY_FLOW_PLAYING_PREDICTION, s_runtime.gap23_ms)) {
                action->id = APP_MODE_ACTION_RETURN_IDLE;
                action->led.scene_id = (uint32_t)CONFIG_ORB_OFFLINE_PROPHECY_SCENE_IDLE_ID;
                reset_flow();
                ESP_LOGW(TAG, "offline.prophecy failed to queue next phase from understanding");
            }
            break;
        }
        if (s_flow == PROPHECY_FLOW_PLAYING_PREDICTION) {
            if (!queue_next_phase_action(action, PROPHECY_FLOW_PLAYING_FAREWELL, s_runtime.gap34_ms)) {
                action->id = APP_MODE_ACTION_RETURN_IDLE;
                action->led.scene_id = (uint32_t)CONFIG_ORB_OFFLINE_PROPHECY_SCENE_IDLE_ID;
                reset_flow();
                ESP_LOGW(TAG, "offline.prophecy failed to queue next phase from prediction");
            }
            break;
        }
        if (s_flow == PROPHECY_FLOW_PLAYING_FAREWELL) {
            action->id = APP_MODE_ACTION_AUDIO_BG_FADE_OUT;
            action->bg.fade_ms = s_runtime.bg_fade_out_ms;
            s_flow = PROPHECY_FLOW_WAIT_BG_FADE_OUT;
            ESP_LOGI(TAG, "offline.prophecy farewell done -> bg fade-out");
        }
        break;

    case APP_MODE_EVENT_TIMER_EXPIRED:
        break;

    case APP_MODE_EVENT_AUDIO_ERROR:
        if (s_flow == PROPHECY_FLOW_IDLE) {
            break;
        }
        action->id = APP_MODE_ACTION_RETURN_IDLE;
        action->led.scene_id = (uint32_t)CONFIG_ORB_OFFLINE_PROPHECY_SCENE_IDLE_ID;
        reset_flow();
        ESP_LOGW(TAG, "offline.prophecy audio error -> idle");
        break;

    default:
        break;
    }

    return ESP_OK;
}

const offline_submode_handler_t *offline_submode_prophecy_get(void)
{
    static const offline_submode_handler_t handler = {
        .id = ORB_OFFLINE_SUBMODE_PROPHECY,
        .name = "prophecy",
        .init = prophecy_init,
        .enter = prophecy_enter,
        .exit = prophecy_exit,
        .handle_event = prophecy_handle_event,
    };
    return &handler;
}
