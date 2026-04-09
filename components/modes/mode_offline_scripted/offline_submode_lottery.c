#include "offline_submode.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
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
    LOTTERY_AUDIO_ASSET_DYNAMIC_SLOT1 = 1001,
};

#define LOTTERY_MAX_PARTICIPANTS 128U

typedef enum {
    LOTTERY_FLOW_IDLE = 0,
    LOTTERY_FLOW_SORTING_AUDIO,
    LOTTERY_FLOW_TEAM_AUDIO,
    LOTTERY_FLOW_TEAM_TTS,
    LOTTERY_FLOW_FINISHED_AUDIO,
    LOTTERY_FLOW_FINISHED_TTS,
} lottery_flow_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t audio_kind;
    uint32_t default_asset_id;
    char track_path[ORB_CONFIG_PATH_MAX];
    char tts_text[ORB_CONFIG_PATH_MAX];
} lottery_team_profile_t;

typedef struct {
    uint8_t audio_kind;
    uint32_t default_asset_id;
    char value[ORB_CONFIG_PATH_MAX];
} lottery_finish_profile_t;

static const lottery_team_profile_t s_default_team_profiles[ORB_LOTTERY_MAX_TEAMS] = {
    { 255U, 60U, 40U, (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_ASSET, LOTTERY_AUDIO_ASSET_TEAM1, "", "" },
    { 40U, 255U, 120U, (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_ASSET, LOTTERY_AUDIO_ASSET_TEAM2, "", "" },
    { 70U, 160U, 255U, (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_ASSET, LOTTERY_AUDIO_ASSET_TEAM3, "", "" },
    { 255U, 210U, 40U, (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_ASSET, LOTTERY_AUDIO_ASSET_TEAM4, "", "" },
};
static const char *LOTTERY_FINISH_TTS_DEFAULT = "Лотерея завершена.";

static lottery_flow_t s_flow;
static TickType_t s_next_allowed_tick;
static uint8_t s_pending_team_idx;
static uint8_t s_round_team_count;
static uint16_t s_round_total;
static uint16_t s_assigned_count;
static uint8_t s_team_bag[LOTTERY_MAX_PARTICIPANTS];
static lottery_team_profile_t s_round_team_profiles[ORB_LOTTERY_MAX_TEAMS];
static lottery_finish_profile_t s_round_finish_profile;
static uint32_t s_pending_team_done_value;
static bool s_round_open;
static uint32_t s_last_start_seq;

static void handle_sorting_done(app_mode_action_t *action);

static uint32_t lottery_scene_idle(void)
{
    return ORB_LED_SCENE_ID_LOTTERY_IDLE;
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
    if (count > ORB_LOTTERY_MAX_TEAMS) {
        count = ORB_LOTTERY_MAX_TEAMS;
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

static void copy_cstr(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, dst_len, "%s", src);
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

static void load_round_settings(void)
{
    const uint8_t fallback_teams = teams_count();
    const uint16_t fallback_total = participants_total();

    s_round_team_count = fallback_teams;
    s_round_total = fallback_total;
    for (uint8_t i = 0U; i < ORB_LOTTERY_MAX_TEAMS; ++i) {
        s_round_team_profiles[i] = s_default_team_profiles[i];
    }
    s_round_finish_profile.audio_kind = (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_ASSET;
    s_round_finish_profile.default_asset_id = LOTTERY_AUDIO_ASSET_FINISHED;
    s_round_finish_profile.value[0] = '\0';

    uint8_t cfg_teams = fallback_teams;
    uint16_t cfg_total = fallback_total;
    if (config_manager_get_offline_lottery_params(&cfg_teams, &cfg_total) != ESP_OK) {
        return;
    }

    if (cfg_teams < 2U || cfg_teams > ORB_LOTTERY_MAX_TEAMS) {
        cfg_teams = fallback_teams;
    }
    s_round_team_count = cfg_teams;

    if (cfg_total == 0U || cfg_total > LOTTERY_MAX_PARTICIPANTS) {
        cfg_total = fallback_total;
    }
    s_round_total = cfg_total;

    for (uint8_t i = 0U; i < ORB_LOTTERY_MAX_TEAMS; ++i) {
        orb_lottery_team_config_t team_cfg = { 0 };
        if (config_manager_get_offline_lottery_team(i, &team_cfg) != ESP_OK) {
            continue;
        }
        lottery_team_profile_t *dst = &s_round_team_profiles[i];
        dst->r = team_cfg.color_r;
        dst->g = team_cfg.color_g;
        dst->b = team_cfg.color_b;
        dst->default_asset_id = s_default_team_profiles[i].default_asset_id;
        dst->audio_kind = (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_ASSET;
        dst->track_path[0] = '\0';
        dst->tts_text[0] = '\0';

        if (team_cfg.source == (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TTS && team_cfg.tts_text[0] != '\0') {
            dst->audio_kind = (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_TTS;
            copy_cstr(dst->tts_text, sizeof(dst->tts_text), team_cfg.tts_text);
        } else if (team_cfg.track_path[0] != '\0') {
            dst->audio_kind = (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_TRACK_PATH;
            copy_cstr(dst->track_path, sizeof(dst->track_path), team_cfg.track_path);
        }
    }

    uint8_t finish_source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK;
    char finish_value[ORB_CONFIG_PATH_MAX] = { 0 };
    if (config_manager_get_offline_lottery_finish(&finish_source, finish_value, sizeof(finish_value)) == ESP_OK) {
        if (finish_source == (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TTS) {
            s_round_finish_profile.audio_kind = (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_TTS;
            if (finish_value[0] != '\0') {
                copy_cstr(s_round_finish_profile.value, sizeof(s_round_finish_profile.value), finish_value);
            } else {
                copy_cstr(s_round_finish_profile.value, sizeof(s_round_finish_profile.value), LOTTERY_FINISH_TTS_DEFAULT);
            }
        } else if (finish_value[0] != '\0') {
            s_round_finish_profile.audio_kind = (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_TRACK_PATH;
            copy_cstr(s_round_finish_profile.value, sizeof(s_round_finish_profile.value), finish_value);
        }
    }
    ESP_LOGI(TAG,
             "offline.lottery finish config: source=%u kind=%u value='%s'",
             (unsigned)finish_source,
             (unsigned)s_round_finish_profile.audio_kind,
             s_round_finish_profile.value);
}

static void build_round_bag(void)
{
    load_round_settings();
    const uint8_t teams = s_round_team_count;
    const uint16_t total = s_round_total;
    uint16_t quotas[4] = { 0 };

    const uint16_t base = (uint16_t)(total / teams);
    uint8_t rem = (uint8_t)(total % teams);
    for (uint8_t i = 0; i < teams; ++i) {
        quotas[i] = base;
    }

    /* Remainder goes to one random team, as requested by product flow. */
    if (rem > 0U) {
        uint8_t rem_team = (uint8_t)(esp_random() % teams);
        quotas[rem_team] = (uint16_t)(quotas[rem_team] + rem);
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
    s_pending_team_done_value = 0U;

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

static bool round_all_teams_tts(void)
{
    uint8_t teams = s_round_team_count;
    if (teams < 2U) {
        teams = 2U;
    }
    if (teams > ORB_LOTTERY_MAX_TEAMS) {
        teams = ORB_LOTTERY_MAX_TEAMS;
    }
    for (uint8_t i = 0U; i < teams; ++i) {
        if (s_round_team_profiles[i].audio_kind != (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_TTS) {
            return false;
        }
    }
    return true;
}

static bool maybe_apply_start_request(app_mode_action_t *action)
{
    if (action == NULL) {
        return false;
    }

    uint32_t start_seq = 0U;
    if (config_manager_get_offline_lottery_start_seq(&start_seq) != ESP_OK) {
        return false;
    }
    if (start_seq == s_last_start_seq) {
        return false;
    }
    if (s_flow != LOTTERY_FLOW_IDLE) {
        /* Keep request pending until we are back in idle. */
        return false;
    }

    s_last_start_seq = start_seq;
    build_round_bag();
    s_round_open = true;
    s_next_allowed_tick = xTaskGetTickCount();
    action->id = APP_MODE_ACTION_LOTTERY_RETURN_IDLE;
    action->led.scene_id = lottery_scene_idle();
    action->led.duration_ms = 0U;
    ESP_LOGI(TAG, "offline.lottery start requested via web");
    return true;
}

static esp_err_t lottery_init(void)
{
    s_flow = LOTTERY_FLOW_IDLE;
    s_next_allowed_tick = 0U;
    s_round_open = false;
    s_round_team_count = teams_count();
    s_pending_team_done_value = 0U;
    s_last_start_seq = 0U;
    (void)config_manager_get_offline_lottery_start_seq(&s_last_start_seq);
    ESP_LOGI(TAG, "offline.lottery init");
    return ESP_OK;
}

static esp_err_t lottery_enter(void)
{
    s_flow = LOTTERY_FLOW_IDLE;
    s_next_allowed_tick = 0U;
    s_round_open = false;
    s_round_team_count = teams_count();
    s_pending_team_done_value = 0U;
    (void)config_manager_get_offline_lottery_start_seq(&s_last_start_seq);
    ESP_LOGI(TAG, "offline.lottery enter (waiting web start)");
    return ESP_OK;
}

static esp_err_t lottery_exit(void)
{
    s_flow = LOTTERY_FLOW_IDLE;
    s_round_open = false;
    s_pending_team_done_value = 0U;
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

    if (round_all_teams_tts()) {
        /* Pure-TTS lottery: no sorting MP3 step. */
        handle_sorting_done(action);
        return ESP_OK;
    }

    action->id = APP_MODE_ACTION_LOTTERY_START_SORTING;
    action->led.scene_id = lottery_scene_idle();
    action->led.duration_ms = 0U;
    action->audio.asset_id = LOTTERY_AUDIO_ASSET_SORTING;
    s_flow = LOTTERY_FLOW_SORTING_AUDIO;

    ESP_LOGI(TAG, "offline.lottery sorting start: assigned=%u/%u", (unsigned)s_assigned_count, (unsigned)s_round_total);
    return ESP_OK;
}

static void complete_finished_round(app_mode_action_t *action)
{
    s_flow = LOTTERY_FLOW_IDLE;
    s_round_open = false;
    s_pending_team_done_value = 0U;
    s_next_allowed_tick = xTaskGetTickCount() + ms_to_ticks_min1((uint32_t)CONFIG_ORB_OFFLINE_LOTTERY_TRIGGER_DEBOUNCE_MS);
    action->id = APP_MODE_ACTION_LOTTERY_RETURN_IDLE;
    action->led.scene_id = lottery_scene_idle();
    action->led.duration_ms = 0U;
    ESP_LOGI(TAG, "offline.lottery finished prompt done -> wait web start");
}

static void start_finished_prompt(app_mode_action_t *action)
{
    action->id = APP_MODE_ACTION_LOTTERY_START_SORTING;
    action->led.scene_id = lottery_scene_idle();
    action->led.duration_ms = 0U;
    action->audio.asset_id = s_round_finish_profile.default_asset_id;
    action->audio.asset_id_2 = (uint32_t)s_round_finish_profile.audio_kind;
    action->mic.tts_timeout_ms = 90000U;
    if (s_round_finish_profile.audio_kind == (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_TTS) {
        copy_cstr(action->mic.tts_text, sizeof(action->mic.tts_text), s_round_finish_profile.value);
        s_pending_team_done_value = 0U;
        s_flow = LOTTERY_FLOW_FINISHED_TTS;
    } else if (s_round_finish_profile.audio_kind == (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_TRACK_PATH) {
        copy_cstr(action->mic.tts_text, sizeof(action->mic.tts_text), s_round_finish_profile.value);
        action->audio.asset_id = LOTTERY_AUDIO_ASSET_DYNAMIC_SLOT1;
        s_pending_team_done_value = LOTTERY_AUDIO_ASSET_DYNAMIC_SLOT1;
        s_flow = LOTTERY_FLOW_FINISHED_AUDIO;
    } else {
        action->mic.tts_text[0] = '\0';
        s_pending_team_done_value = LOTTERY_AUDIO_ASSET_FINISHED;
        s_flow = LOTTERY_FLOW_FINISHED_AUDIO;
    }
}

static void handle_sorting_done(app_mode_action_t *action)
{
    if (!round_has_next()) {
        start_finished_prompt(action);
        return;
    }

    s_pending_team_idx = round_pop_next_team();
    if (s_pending_team_idx > 3U) {
        s_pending_team_idx = 0U;
    }
    const lottery_team_profile_t *team = &s_round_team_profiles[s_pending_team_idx];

    action->id = APP_MODE_ACTION_LOTTERY_ASSIGN_TEAM;
    action->led.scene_id = ORB_LED_SCENE_ID_LOTTERY_TEAM_COLOR;
    action->led.duration_ms = 0U;
    action->led.color_r = team->r;
    action->led.color_g = team->g;
    action->led.color_b = team->b;
    action->audio.asset_id = team->default_asset_id;
    action->audio.asset_id_2 = (uint32_t)team->audio_kind;
    action->mic.tts_timeout_ms = 90000U;

    if (team->audio_kind == (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_TTS) {
        copy_cstr(action->mic.tts_text, sizeof(action->mic.tts_text), team->tts_text);
        s_pending_team_done_value = 0U;
        s_flow = LOTTERY_FLOW_TEAM_TTS;
    } else if (team->audio_kind == (uint8_t)APP_MODE_LOTTERY_AUDIO_KIND_TRACK_PATH) {
        copy_cstr(action->mic.tts_text, sizeof(action->mic.tts_text), team->track_path);
        action->audio.asset_id = LOTTERY_AUDIO_ASSET_DYNAMIC_SLOT1;
        s_pending_team_done_value = LOTTERY_AUDIO_ASSET_DYNAMIC_SLOT1;
        s_flow = LOTTERY_FLOW_TEAM_AUDIO;
    } else {
        action->mic.tts_text[0] = '\0';
        s_pending_team_done_value = action->audio.asset_id;
        s_flow = LOTTERY_FLOW_TEAM_AUDIO;
    }

    ESP_LOGI(TAG,
             "offline.lottery assigned team=%u kind=%u progress=%u/%u",
             (unsigned)s_pending_team_idx,
             (unsigned)team->audio_kind,
             (unsigned)s_assigned_count,
             (unsigned)s_round_total);
}

static void handle_team_audio_done(app_mode_action_t *action)
{
    TickType_t now = xTaskGetTickCount();

    if (s_assigned_count >= s_round_total) {
        start_finished_prompt(action);
        ESP_LOGI(TAG, "offline.lottery round complete -> finished phrase");
        return;
    }

    s_flow = LOTTERY_FLOW_IDLE;
    s_pending_team_done_value = 0U;
    s_next_allowed_tick = now + ms_to_ticks_min1((uint32_t)CONFIG_ORB_OFFLINE_LOTTERY_RESULT_COOLDOWN_MS);
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
    if (maybe_apply_start_request(action)) {
        return ESP_OK;
    }

    switch (event->id) {
    case APP_MODE_EVENT_TOUCH_HOLD:
        return handle_touch_hold(action);
    case APP_MODE_EVENT_AUDIO_DONE:
        if (s_flow == LOTTERY_FLOW_SORTING_AUDIO && event->value == LOTTERY_AUDIO_ASSET_SORTING) {
            handle_sorting_done(action);
        } else if (s_flow == LOTTERY_FLOW_TEAM_AUDIO && event->code == (int32_t)APP_MODE_AUDIO_DONE_CODE_NONE &&
                   event->value == s_pending_team_done_value) {
            handle_team_audio_done(action);
        } else if (s_flow == LOTTERY_FLOW_FINISHED_AUDIO && event->code == (int32_t)APP_MODE_AUDIO_DONE_CODE_NONE &&
                   event->value == s_pending_team_done_value) {
            complete_finished_round(action);
        }
        break;
    case APP_MODE_EVENT_MIC_TTS_DONE:
        if (s_flow == LOTTERY_FLOW_TEAM_TTS) {
            handle_team_audio_done(action);
        } else if (s_flow == LOTTERY_FLOW_FINISHED_TTS) {
            complete_finished_round(action);
        }
        break;
    case APP_MODE_EVENT_MIC_TTS_ERROR:
        if (s_flow == LOTTERY_FLOW_TEAM_TTS) {
            ESP_LOGW(TAG, "offline.lottery team tts error -> continue");
            handle_team_audio_done(action);
        } else if (s_flow == LOTTERY_FLOW_FINISHED_TTS) {
            ESP_LOGW(TAG, "offline.lottery finish tts error -> close round");
            complete_finished_round(action);
        }
        break;
    case APP_MODE_EVENT_AUDIO_ERROR:
        if (s_flow != LOTTERY_FLOW_IDLE) {
            s_flow = LOTTERY_FLOW_IDLE;
            s_pending_team_done_value = 0U;
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
