#include "offline_submode.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "orb_led_scenes.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MODE_OFFLINE;

#ifndef CONFIG_ORB_OFFLINE_GRUMBLE_ASSET_ID
#define CONFIG_ORB_OFFLINE_GRUMBLE_ASSET_ID 3
#endif
#ifndef CONFIG_ORB_OFFLINE_GRUMBLE_ENABLE
#define CONFIG_ORB_OFFLINE_GRUMBLE_ENABLE 1
#endif
#ifndef CONFIG_ORB_OFFLINE_GRUMBLE_MIN_TOUCHES
#define CONFIG_ORB_OFFLINE_GRUMBLE_MIN_TOUCHES 3
#endif
#ifndef CONFIG_ORB_OFFLINE_GRUMBLE_WINDOW_MS
#define CONFIG_ORB_OFFLINE_GRUMBLE_WINDOW_MS 6000
#endif
#ifndef CONFIG_ORB_OFFLINE_GRUMBLE_COOLDOWN_MS
#define CONFIG_ORB_OFFLINE_GRUMBLE_COOLDOWN_MS 20000
#endif
#ifndef CONFIG_ORB_OFFLINE_GRUMBLE_BASE_PROB_PCT
#define CONFIG_ORB_OFFLINE_GRUMBLE_BASE_PROB_PCT 15
#endif
#ifndef CONFIG_ORB_OFFLINE_GRUMBLE_STEP_PROB_PCT
#define CONFIG_ORB_OFFLINE_GRUMBLE_STEP_PROB_PCT 10
#endif
#ifndef CONFIG_ORB_OFFLINE_GRUMBLE_MAX_PROB_PCT
#define CONFIG_ORB_OFFLINE_GRUMBLE_MAX_PROB_PCT 65
#endif
#if defined(CONFIG_ORB_OFFLINE_GRUMBLE_ASSET_ID)
#define ORB_OFFLINE_GRUMBLE_ASSET_ID_U32 ((uint32_t)CONFIG_ORB_OFFLINE_GRUMBLE_ASSET_ID)
#else
#define ORB_OFFLINE_GRUMBLE_ASSET_ID_U32 UINT32_C(3)
#endif

enum {
    OFFLINE_SCENE_IDLE = ORB_LED_SCENE_ID_FIRE2012,
    OFFLINE_SCENE_GRUMBLE = ORB_LED_SCENE_ID_GRUMBLE_RED,
    OFFLINE_SCENE_AURA_ACTIVATING = ORB_LED_SCENE_ID_PLASMA,
    OFFLINE_SCENE_AURA_COLOR = ORB_LED_SCENE_ID_AURA_COLOR_BREATHE,
    OFFLINE_AURA_POST_DELAY_MS = 5000U,
    OFFLINE_AURA_FADE_OUT_MS = 1000U,
};

typedef enum {
    OFFLINE_AURA_FLOW_IDLE = 0,
    OFFLINE_AURA_FLOW_GRUMBLE,
    OFFLINE_AURA_FLOW_SEQUENCE,
    OFFLINE_AURA_FLOW_POST_DELAY,
    OFFLINE_AURA_FLOW_FADE_OUT,
} offline_aura_flow_t;

typedef struct {
    TickType_t last_touch_tick;
    TickType_t cooldown_until_tick;
    uint16_t touches_accum;
} grumble_policy_t;

static offline_aura_flow_t s_aura_flow;
static grumble_policy_t s_grumble_policy;

#if CONFIG_ORB_OFFLINE_GRUMBLE_ENABLE
static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static bool time_reached(TickType_t now, TickType_t deadline)
{
    return ((int32_t)(now - deadline) >= 0);
}
#endif

static void grumble_policy_reset(void)
{
    s_grumble_policy.last_touch_tick = 0;
    s_grumble_policy.cooldown_until_tick = 0;
    s_grumble_policy.touches_accum = 0U;
}

static bool grumble_should_trigger_on_touch(void)
{
#if !CONFIG_ORB_OFFLINE_GRUMBLE_ENABLE
    return false;
#else
    TickType_t now = xTaskGetTickCount();
    TickType_t window_ticks = ms_to_ticks_min1((uint32_t)CONFIG_ORB_OFFLINE_GRUMBLE_WINDOW_MS);

    if (s_grumble_policy.last_touch_tick != 0U && time_reached(now, s_grumble_policy.last_touch_tick + window_ticks)) {
        s_grumble_policy.touches_accum = 0U;
    }
    s_grumble_policy.last_touch_tick = now;

    if (s_grumble_policy.touches_accum < UINT16_MAX) {
        s_grumble_policy.touches_accum++;
    }

    if (!time_reached(now, s_grumble_policy.cooldown_until_tick)) {
        return false;
    }

    if (s_grumble_policy.touches_accum < (uint16_t)CONFIG_ORB_OFFLINE_GRUMBLE_MIN_TOUCHES) {
        return false;
    }

    uint32_t chance_pct = (uint32_t)CONFIG_ORB_OFFLINE_GRUMBLE_BASE_PROB_PCT;
    uint32_t extra_touches = (uint32_t)s_grumble_policy.touches_accum - (uint32_t)CONFIG_ORB_OFFLINE_GRUMBLE_MIN_TOUCHES;
    chance_pct += extra_touches * (uint32_t)CONFIG_ORB_OFFLINE_GRUMBLE_STEP_PROB_PCT;
    if (chance_pct > (uint32_t)CONFIG_ORB_OFFLINE_GRUMBLE_MAX_PROB_PCT) {
        chance_pct = (uint32_t)CONFIG_ORB_OFFLINE_GRUMBLE_MAX_PROB_PCT;
    }
    if (chance_pct > 100U) {
        chance_pct = 100U;
    }

    uint32_t roll = esp_random() % 100U;
    if (roll >= chance_pct) {
        return false;
    }

    s_grumble_policy.cooldown_until_tick = now + ms_to_ticks_min1((uint32_t)CONFIG_ORB_OFFLINE_GRUMBLE_COOLDOWN_MS);
    s_grumble_policy.touches_accum = 0U;
    ESP_LOGI(TAG, "offline.aura: grumble triggered (chance=%" PRIu32 "%% roll=%" PRIu32 ")", chance_pct, roll);
    return true;
#endif
}

static esp_err_t aura_init(void)
{
    s_aura_flow = OFFLINE_AURA_FLOW_IDLE;
    grumble_policy_reset();
#if CONFIG_ORB_OFFLINE_GRUMBLE_ENABLE
    ESP_LOGI(TAG,
             "offline.aura grumble enabled asset=%" PRIu32 " min_touches=%d window=%dms cooldown=%dms prob=%d+N*%d max=%d",
             ORB_OFFLINE_GRUMBLE_ASSET_ID_U32,
             CONFIG_ORB_OFFLINE_GRUMBLE_MIN_TOUCHES,
             CONFIG_ORB_OFFLINE_GRUMBLE_WINDOW_MS,
             CONFIG_ORB_OFFLINE_GRUMBLE_COOLDOWN_MS,
             CONFIG_ORB_OFFLINE_GRUMBLE_BASE_PROB_PCT,
             CONFIG_ORB_OFFLINE_GRUMBLE_STEP_PROB_PCT,
             CONFIG_ORB_OFFLINE_GRUMBLE_MAX_PROB_PCT);
#else
    ESP_LOGI(TAG, "offline.aura grumble disabled");
#endif
    ESP_LOGI(TAG, "offline.aura init");
    return ESP_OK;
}

static esp_err_t aura_enter(void)
{
    s_aura_flow = OFFLINE_AURA_FLOW_IDLE;
    grumble_policy_reset();
    ESP_LOGI(TAG, "offline.aura enter");
    return ESP_OK;
}

static esp_err_t aura_exit(void)
{
    s_aura_flow = OFFLINE_AURA_FLOW_IDLE;
    grumble_policy_reset();
    ESP_LOGI(TAG, "offline.aura exit");
    return ESP_OK;
}

static esp_err_t aura_handle_event(const app_mode_event_t *event, app_mode_action_t *action)
{
    if (event == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *action = (app_mode_action_t){ 0 };

    switch (event->id) {
    case APP_MODE_EVENT_TOUCH_DOWN:
        if (s_aura_flow != OFFLINE_AURA_FLOW_IDLE) {
            break;
        }
        if (!grumble_should_trigger_on_touch()) {
            break;
        }
        action->id = APP_MODE_ACTION_PLAY_GRUMBLE;
        action->audio.asset_id = ORB_OFFLINE_GRUMBLE_ASSET_ID_U32;
        action->led.scene_id = OFFLINE_SCENE_GRUMBLE;
        s_aura_flow = OFFLINE_AURA_FLOW_GRUMBLE;
        break;
    case APP_MODE_EVENT_TOUCH_HOLD:
        if (s_aura_flow != OFFLINE_AURA_FLOW_IDLE && s_aura_flow != OFFLINE_AURA_FLOW_GRUMBLE) {
            break;
        }
        action->id = APP_MODE_ACTION_START_AUDIO_SEQUENCE;
        action->audio.sequence_kind = APP_MODE_SEQUENCE_AURA;
        action->led.scene_id = OFFLINE_SCENE_AURA_ACTIVATING;
        action->audio.asset_id = (uint32_t)CONFIG_ORB_AURA_TRACK1_ASSET_ID;
        action->audio.asset_id_2 = (uint32_t)CONFIG_ORB_AURA_TRACK2_ASSET_ID;
        action->audio.gap_ms = 0U;
        if (s_aura_flow == OFFLINE_AURA_FLOW_GRUMBLE) {
            ESP_LOGI(TAG, "offline.aura: hold -> preempt grumble and start sequence");
        } else {
            ESP_LOGI(TAG, "offline.aura: hold -> start sequence");
        }
        s_aura_flow = OFFLINE_AURA_FLOW_SEQUENCE;
        break;
    case APP_MODE_EVENT_AUDIO_DONE:
        if (s_aura_flow == OFFLINE_AURA_FLOW_GRUMBLE) {
            if (event->value == ORB_OFFLINE_GRUMBLE_ASSET_ID_U32) {
                s_aura_flow = OFFLINE_AURA_FLOW_IDLE;
                ESP_LOGI(TAG, "offline.aura: grumble done");
            }
            break;
        }
        if (s_aura_flow != OFFLINE_AURA_FLOW_SEQUENCE) {
            break;
        }
        if (event->value != (uint32_t)CONFIG_ORB_AURA_TRACK2_ASSET_ID) {
            break;
        }
        action->id = APP_MODE_ACTION_BEGIN_COOLDOWN;
        action->led.scene_id = OFFLINE_SCENE_AURA_COLOR;
        action->timing.cooldown_ms = OFFLINE_AURA_POST_DELAY_MS;
        s_aura_flow = OFFLINE_AURA_FLOW_POST_DELAY;
        ESP_LOGI(TAG, "offline.aura: track2 done -> post delay");
        break;
    case APP_MODE_EVENT_TIMER_EXPIRED:
        if (s_aura_flow == OFFLINE_AURA_FLOW_POST_DELAY) {
            action->id = APP_MODE_ACTION_AURA_FADE_OUT;
            action->led.fade_ms = OFFLINE_AURA_FADE_OUT_MS;
            action->timing.cooldown_ms = OFFLINE_AURA_FADE_OUT_MS;
            s_aura_flow = OFFLINE_AURA_FLOW_FADE_OUT;
            ESP_LOGI(TAG, "offline.aura: post delay complete -> fade out");
        } else if (s_aura_flow == OFFLINE_AURA_FLOW_FADE_OUT) {
            action->id = APP_MODE_ACTION_RETURN_IDLE;
            action->led.scene_id = OFFLINE_SCENE_IDLE;
            s_aura_flow = OFFLINE_AURA_FLOW_IDLE;
            ESP_LOGI(TAG, "offline.aura: fade complete -> idle");
        }
        break;
    case APP_MODE_EVENT_AUDIO_ERROR:
        action->id = APP_MODE_ACTION_RETURN_IDLE;
        action->led.scene_id = OFFLINE_SCENE_IDLE;
        s_aura_flow = OFFLINE_AURA_FLOW_IDLE;
        ESP_LOGW(TAG, "offline.aura: audio error -> idle");
        break;
    default:
        break;
    }

    return ESP_OK;
}

const offline_submode_handler_t *offline_submode_aura_get(void)
{
    static const offline_submode_handler_t handler = {
        .id = ORB_OFFLINE_SUBMODE_AURA,
        .name = "aura",
        .init = aura_init,
        .enter = aura_enter,
        .exit = aura_exit,
        .handle_event = aura_handle_event,
    };
    return &handler;
}
