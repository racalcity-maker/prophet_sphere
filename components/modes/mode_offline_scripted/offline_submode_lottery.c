#include "offline_submode.h"

#include <stdbool.h>
#include "config_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "orb_led_scenes.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MODE_OFFLINE;

#ifndef CONFIG_ORB_OFFLINE_LOTTERY_TEAMS_COUNT
#define CONFIG_ORB_OFFLINE_LOTTERY_TEAMS_COUNT 4
#endif
#ifndef CONFIG_ORB_OFFLINE_LOTTERY_PARTICIPANTS_TOTAL
#define CONFIG_ORB_OFFLINE_LOTTERY_PARTICIPANTS_TOTAL 17
#endif
#ifndef CONFIG_ORB_OFFLINE_LOTTERY_TOUCH_DEBOUNCE_MS
#define CONFIG_ORB_OFFLINE_LOTTERY_TOUCH_DEBOUNCE_MS 260
#endif
#ifndef CONFIG_ORB_OFFLINE_LOTTERY_TRIGGER_DEBOUNCE_MS
#define CONFIG_ORB_OFFLINE_LOTTERY_TRIGGER_DEBOUNCE_MS 450
#endif
#ifndef CONFIG_ORB_OFFLINE_LOTTERY_RESULT_COOLDOWN_MS
#define CONFIG_ORB_OFFLINE_LOTTERY_RESULT_COOLDOWN_MS 1200
#endif
#ifndef CONFIG_ORB_OFFLINE_LOTTERY_SCENE_IDLE_ID
#define CONFIG_ORB_OFFLINE_LOTTERY_SCENE_IDLE_ID ORB_LED_SCENE_ID_LOTTERY_IDLE
#endif
#ifndef CONFIG_ORB_OFFLINE_LOTTERY_SCENE_SORTING_ID
#define CONFIG_ORB_OFFLINE_LOTTERY_SCENE_SORTING_ID ORB_LED_SCENE_ID_LOTTERY_HOLD_RAMP
#endif
#ifndef CONFIG_ORB_OFFLINE_LOTTERY_SCENE_RESULT_ID
#define CONFIG_ORB_OFFLINE_LOTTERY_SCENE_RESULT_ID ORB_LED_SCENE_ID_AURA_COLOR_BREATHE
#endif

/*
 * Keep asset-id values aligned with services_audio/include/audio_types.h
 * without introducing component dependency cycles.
 */
enum {
    LOTTERY_AUDIO_ASSET_SORTING = 1101,
    LOTTERY_AUDIO_ASSET_TEAM1 = 1102,
    LOTTERY_AUDIO_ASSET_TEAM2 = 1103,
    LOTTERY_AUDIO_ASSET_TEAM3 = 1104,
    LOTTERY_AUDIO_ASSET_TEAM4 = 1105,
    LOTTERY_AUDIO_ASSET_FINISHED = 1106,
};

#define LOTTERY_MAX_PARTICIPANTS 128U

typedef enum {
    LOTTERY_FLOW_IDLE = 0,
    LOTTERY_FLOW_SORTING_AUDIO,
    LOTTERY_FLOW_TEAM_AUDIO,
    LOTTERY_FLOW_FINISHED_AUDIO,
} lottery_flow_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint32_t audio_asset_id;
} lottery_team_profile_t;

static const lottery_team_profile_t s_team_profiles[4] = {
    { 255U, 60U, 40U, LOTTERY_AUDIO_ASSET_TEAM1 },
    { 40U, 255U, 120U, LOTTERY_AUDIO_ASSET_TEAM2 },
    { 70U, 160U, 255U, LOTTERY_AUDIO_ASSET_TEAM3 },
    { 255U, 210U, 40U, LOTTERY_AUDIO_ASSET_TEAM4 },
};

static lottery_flow_t s_flow;
static TickType_t s_next_allowed_tick;
static uint8_t s_pending_team_idx;
static uint16_t s_round_total;
static uint16_t s_assigned_count;
static uint8_t s_team_bag[LOTTERY_MAX_PARTICIPANTS];
static bool s_round_open;
static uint32_t s_last_start_seq;

static uint32_t lottery_scene_idle(void)
{
    return (uint32_t)CONFIG_ORB_OFFLINE_LOTTERY_SCENE_IDLE_ID;
}

static uint32_t lottery_scene_sorting(void)
{
    return (uint32_t)CONFIG_ORB_OFFLINE_LOTTERY_SCENE_SORTING_ID;
}

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0U) ? ticks : 1U;
}

static bool time_reached(TickType_t now, TickType_t deadline)
{
    return ((int32_t)(now - deadline) >= 0);
}

static uint8_t teams_count(void)
{
    uint8_t count = (uint8_t)CONFIG_ORB_OFFLINE_LOTTERY_TEAMS_COUNT;
    if (count < 2U) {
        count = 2U;
    }
    if (count > 4U) {
        count = 4U;
    }
    return count;
}

static uint16_t participants_total(void)
{
    uint16_t total = (uint16_t)CONFIG_ORB_OFFLINE_LOTTERY_PARTICIPANTS_TOTAL;
    if (total == 0U) {
        total = 1U;
    }
    if (total > LOTTERY_MAX_PARTICIPANTS) {
        total = LOTTERY_MAX_PARTICIPANTS;
    }
    return total;
}

static void shuffle_u8(uint8_t *arr, size_t len)
{
    if (arr == NULL || len < 2U) {
        return;
    }
    for (size_t i = len - 1U; i > 0U; --i) {
        size_t j = (size_t)(esp_random() % (uint32_t)(i + 1U));
        uint8_t t = arr[i];
        arr[i] = arr[j];
        arr[j] = t;
    }
}

static void build_round_bag(void)
{
    const uint8_t teams = teams_count();
    const uint16_t total = participants_total();
    uint16_t quotas[4] = { 0 };

    const uint16_t base = (uint16_t)(total / teams);
    uint8_t rem = (uint8_t)(total % teams);
    for (uint8_t i = 0; i < teams; ++i) {
        quotas[i] = base;
    }

    uint8_t indices[4] = { 0, 1, 2, 3 };
    shuffle_u8(indices, teams);
    for (uint8_t i = 0; i < rem; ++i) {
        quotas[indices[i]]++;
    }

    uint16_t pos = 0U;
    for (uint8_t team = 0; team < teams; ++team) {
        for (uint16_t n = 0; n < quotas[team] && pos < LOTTERY_MAX_PARTICIPANTS; ++n) {
            s_team_bag[pos++] = team;
        }
    }
    shuffle_u8(s_team_bag, pos);

    s_round_total = pos;
    s_assigned_count = 0U;
    s_pending_team_idx = 0U;

    ESP_LOGI(TAG,
             "offline.lottery round: total=%u teams=%u quotas=[%u,%u,%u,%u]",
             (unsigned)s_round_total,
             (unsigned)teams,
             (unsigned)quotas[0],
             (unsigned)quotas[1],
             (unsigned)quotas[2],
             (unsigned)quotas[3]);
}

static bool round_has_next(void)
{
    if (!s_round_open) {
        return false;
    }
    return s_assigned_count < s_round_total;
}

static uint8_t round_pop_next_team(void)
{
    if (!round_has_next()) {
        return 0U;
    }
    uint8_t idx = s_team_bag[s_assigned_count];
    s_assigned_count++;
    return idx;
}

static bool is_team_asset(uint32_t asset)
{
    return (asset == LOTTERY_AUDIO_ASSET_TEAM1 || asset == LOTTERY_AUDIO_ASSET_TEAM2 || asset == LOTTERY_AUDIO_ASSET_TEAM3 ||
            asset == LOTTERY_AUDIO_ASSET_TEAM4);
}

static void maybe_apply_start_request(void)
{
    orb_runtime_config_t cfg = { 0 };
    if (config_manager_get_snapshot(&cfg) != ESP_OK) {
        return;
    }
    if (cfg.offline_lottery_start_seq == s_last_start_seq) {
        return;
    }
    if (s_flow != LOTTERY_FLOW_IDLE) {
        /* Keep request pending until we are back in idle. */
        return;
    }

    s_last_start_seq = cfg.offline_lottery_start_seq;
    build_round_bag();
    s_round_open = true;
    s_next_allowed_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "offline.lottery start requested via web");
}

static esp_err_t lottery_init(void)
{
    s_flow = LOTTERY_FLOW_IDLE;
    s_next_allowed_tick = 0U;
    s_round_open = false;
    s_last_start_seq = 0U;
    {
        orb_runtime_config_t cfg = { 0 };
        if (config_manager_get_snapshot(&cfg) == ESP_OK) {
            s_last_start_seq = cfg.offline_lottery_start_seq;
        }
    }
    ESP_LOGI(TAG, "offline.lottery init");
    return ESP_OK;
}

static esp_err_t lottery_enter(void)
{
    s_flow = LOTTERY_FLOW_IDLE;
    s_next_allowed_tick = 0U;
    s_round_open = false;
    {
        orb_runtime_config_t cfg = { 0 };
        if (config_manager_get_snapshot(&cfg) == ESP_OK) {
            s_last_start_seq = cfg.offline_lottery_start_seq;
        }
    }
    ESP_LOGI(TAG, "offline.lottery enter (waiting web start)");
    return ESP_OK;
}

static esp_err_t lottery_exit(void)
{
    s_flow = LOTTERY_FLOW_IDLE;
    s_round_open = false;
    ESP_LOGI(TAG, "offline.lottery exit");
    return ESP_OK;
}

static esp_err_t handle_touch_hold(app_mode_action_t *action)
{
    if (s_flow != LOTTERY_FLOW_IDLE) {
        return ESP_OK;
    }

    TickType_t now = xTaskGetTickCount();
    if (!time_reached(now, s_next_allowed_tick)) {
        return ESP_OK;
    }
    if (!round_has_next()) {
        return ESP_OK;
    }

    action->id = APP_MODE_ACTION_LOTTERY_START_SORTING;
    action->led.scene_id = lottery_scene_sorting();
    action->led.duration_ms = 0U;
    action->audio.asset_id = LOTTERY_AUDIO_ASSET_SORTING;
    s_flow = LOTTERY_FLOW_SORTING_AUDIO;

    ESP_LOGI(TAG, "offline.lottery sorting start: assigned=%u/%u", (unsigned)s_assigned_count, (unsigned)s_round_total);
    return ESP_OK;
}

static void handle_sorting_done(app_mode_action_t *action)
{
    if (!round_has_next()) {
        action->id = APP_MODE_ACTION_LOTTERY_START_SORTING;
        action->led.scene_id = lottery_scene_idle();
        action->audio.asset_id = LOTTERY_AUDIO_ASSET_FINISHED;
        action->led.duration_ms = 0U;
        s_flow = LOTTERY_FLOW_FINISHED_AUDIO;
        return;
    }

    s_pending_team_idx = round_pop_next_team();
    if (s_pending_team_idx > 3U) {
        s_pending_team_idx = 0U;
    }
    const lottery_team_profile_t *team = &s_team_profiles[s_pending_team_idx];

    action->id = APP_MODE_ACTION_LOTTERY_ASSIGN_TEAM;
    action->led.scene_id = (uint32_t)CONFIG_ORB_OFFLINE_LOTTERY_SCENE_RESULT_ID;
    action->led.duration_ms = 0U;
    action->audio.asset_id = team->audio_asset_id;
    action->led.color_r = team->r;
    action->led.color_g = team->g;
    action->led.color_b = team->b;
    s_flow = LOTTERY_FLOW_TEAM_AUDIO;

    ESP_LOGI(TAG,
             "offline.lottery assigned team=%u progress=%u/%u",
             (unsigned)s_pending_team_idx,
             (unsigned)s_assigned_count,
             (unsigned)s_round_total);
}

static void handle_team_audio_done(app_mode_action_t *action)
{
    TickType_t now = xTaskGetTickCount();

    if (s_assigned_count >= s_round_total) {
        action->id = APP_MODE_ACTION_LOTTERY_START_SORTING;
        action->led.scene_id = lottery_scene_idle();
        action->audio.asset_id = LOTTERY_AUDIO_ASSET_FINISHED;
        action->led.duration_ms = 0U;
        s_flow = LOTTERY_FLOW_FINISHED_AUDIO;
        ESP_LOGI(TAG, "offline.lottery round complete -> finished phrase");
        return;
    }

    s_flow = LOTTERY_FLOW_IDLE;
    s_next_allowed_tick = now + ms_to_ticks_min1((uint32_t)CONFIG_ORB_OFFLINE_LOTTERY_TRIGGER_DEBOUNCE_MS +
                                                  (uint32_t)CONFIG_ORB_OFFLINE_LOTTERY_RESULT_COOLDOWN_MS);
    action->id = APP_MODE_ACTION_LOTTERY_RETURN_IDLE;
    action->led.scene_id = lottery_scene_idle();
    action->led.duration_ms = 0U;
}

static esp_err_t lottery_handle_event(const app_mode_event_t *event, app_mode_action_t *action)
{
    if (event == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *action = (app_mode_action_t){ 0 };
    maybe_apply_start_request();

    switch (event->id) {
    case APP_MODE_EVENT_TOUCH_HOLD:
        return handle_touch_hold(action);
    case APP_MODE_EVENT_AUDIO_DONE:
        if (s_flow == LOTTERY_FLOW_SORTING_AUDIO && event->value == LOTTERY_AUDIO_ASSET_SORTING) {
            handle_sorting_done(action);
        } else if (s_flow == LOTTERY_FLOW_TEAM_AUDIO && is_team_asset(event->value)) {
            handle_team_audio_done(action);
        } else if (s_flow == LOTTERY_FLOW_FINISHED_AUDIO && event->value == LOTTERY_AUDIO_ASSET_FINISHED) {
            s_flow = LOTTERY_FLOW_IDLE;
            s_round_open = false;
            s_next_allowed_tick = xTaskGetTickCount() + ms_to_ticks_min1((uint32_t)CONFIG_ORB_OFFLINE_LOTTERY_TRIGGER_DEBOUNCE_MS);
            action->id = APP_MODE_ACTION_LOTTERY_RETURN_IDLE;
            action->led.scene_id = lottery_scene_idle();
            action->led.duration_ms = 0U;
            ESP_LOGI(TAG, "offline.lottery finished phrase done -> wait web start");
        }
        break;
    case APP_MODE_EVENT_AUDIO_ERROR:
        if (s_flow != LOTTERY_FLOW_IDLE) {
            s_flow = LOTTERY_FLOW_IDLE;
            s_next_allowed_tick = xTaskGetTickCount() + ms_to_ticks_min1((uint32_t)CONFIG_ORB_OFFLINE_LOTTERY_TRIGGER_DEBOUNCE_MS);
            action->id = APP_MODE_ACTION_LOTTERY_ABORT;
            action->led.scene_id = lottery_scene_idle();
            action->led.duration_ms = 0U;
        }
        break;
    default:
        break;
    }

    return ESP_OK;
}

const offline_submode_handler_t *offline_submode_lottery_get(void)
{
    static const offline_submode_handler_t handler = {
        .id = ORB_OFFLINE_SUBMODE_LOTTERY,
        .name = "lottery",
        .init = lottery_init,
        .enter = lottery_enter,
        .exit = lottery_exit,
        .handle_event = lottery_handle_event,
    };
    return &handler;
}
