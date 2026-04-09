#include "config_manager.h"

#include <inttypes.h>
#include <string.h>
#include "config_defaults.h"
#include "config_store_nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_tags.h"
#include "orb_led_scenes.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_CONFIG;

static orb_runtime_config_t s_snapshot;
static SemaphoreHandle_t s_cfg_mutex;

#ifndef CONFIG_ORB_CONFIG_PERSIST_TO_NVS
#define CONFIG_ORB_CONFIG_PERSIST_TO_NVS 1
#endif

static esp_err_t lock_cfg(void)
{
    if (s_cfg_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t save_snapshot_unsafe(void)
{
#if CONFIG_ORB_CONFIG_PERSIST_TO_NVS
    return config_store_nvs_save(&s_snapshot);
#else
    return ESP_OK;
#endif
}

static bool offline_submode_valid(orb_offline_submode_t submode)
{
    return submode >= ORB_OFFLINE_SUBMODE_AURA && submode <= ORB_OFFLINE_SUBMODE_PROPHECY;
}

static bool prophecy_timing_valid(uint32_t gap12_ms, uint32_t gap23_ms, uint32_t gap34_ms, uint32_t leadin_wait_ms)
{
    return gap12_ms <= 60000U && gap23_ms <= 60000U && gap34_ms <= 60000U && leadin_wait_ms <= 60000U;
}

static bool prophecy_background_valid(uint16_t gain_permille, uint32_t fade_in_ms, uint32_t fade_out_ms)
{
    return gain_permille <= 2000U && fade_in_ms <= 60000U && fade_out_ms <= 60000U;
}

static bool hybrid_params_valid(uint16_t reject_threshold_permille, uint32_t mic_capture_ms)
{
    return reject_threshold_permille <= 1000U && mic_capture_ms >= 1000U && mic_capture_ms <= 60000U;
}

static bool hybrid_unknown_retry_valid(uint8_t unknown_retry_max)
{
    return unknown_retry_max <= 2U;
}

static bool hybrid_effect_scene_valid(uint32_t scene_id)
{
    switch (scene_id) {
    case ORB_LED_SCENE_ID_HYBRID_IDLE_SLOW_BREATHE:
    case ORB_LED_SCENE_ID_HYBRID_TOUCH_FAST_BREATHE:
    case ORB_LED_SCENE_ID_HYBRID_VORTEX:
    case ORB_LED_SCENE_ID_HYBRID_VORTEX_DIM:
    case ORB_LED_SCENE_ID_HYBRID_WLED_METABALLS:
    case ORB_LED_SCENE_ID_HYBRID_WLED_DNA:
    case ORB_LED_SCENE_ID_HYBRID_WLED_TWISTER:
    case ORB_LED_SCENE_ID_HYBRID_WLED_CHECKER_PULSE:
    case ORB_LED_SCENE_ID_HYBRID_WLED_RAIN:
    case ORB_LED_SCENE_ID_HYBRID_WLED_RADIAL_BURST:
    case ORB_LED_SCENE_ID_HYBRID_WLED_TUNNEL:
    case ORB_LED_SCENE_ID_HYBRID_WLED_BANDS:
    case ORB_LED_SCENE_ID_HYBRID_WLED_STARFIELD:
    case ORB_LED_SCENE_ID_HYBRID_WLED_CONFETTI:
    case ORB_LED_SCENE_ID_HYBRID_WLED_LAVA:
    case ORB_LED_SCENE_ID_HYBRID_WLED_RINGS:
    case ORB_LED_SCENE_ID_HYBRID_WLED_NOISE:
    case ORB_LED_SCENE_ID_HYBRID_WLED_SCANNER:
    case ORB_LED_SCENE_ID_HYBRID_WLED_ZIGZAG:
    case ORB_LED_SCENE_ID_HYBRID_WLED_AURORA:
    case ORB_LED_SCENE_ID_HYBRID_WLED_PRISM:
    case ORB_LED_SCENE_ID_HYBRID_WLED_CLOUDS:
    case ORB_LED_SCENE_ID_HYBRID_WLED_WAVEGRID:
    case ORB_LED_SCENE_ID_HYBRID_WLED_HEARTBEAT:
    case ORB_LED_SCENE_ID_HYBRID_WLED_PINWHEEL:
    case ORB_LED_SCENE_ID_HYBRID_WLED_COMET:
        return true;
    default:
        return false;
    }
}

static bool hybrid_effect_valid(uint32_t idle_scene_id,
                                uint32_t talk_scene_id,
                                uint8_t speed,
                                uint8_t intensity,
                                uint8_t scale)
{
    (void)speed;
    (void)intensity;
    (void)scale;
    return hybrid_effect_scene_valid(idle_scene_id) && hybrid_effect_scene_valid(talk_scene_id);
}

static bool hybrid_effect_palette_mode_valid(uint8_t palette_mode)
{
    return palette_mode <= (uint8_t)ORB_LED_PALETTE_MODE_TRIO;
}

static bool lottery_team_source_valid(uint8_t source)
{
    return source <= (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TTS;
}

static size_t bounded_strnlen_local(const char *value, size_t max_len)
{
    if (value == NULL) {
        return 0U;
    }
    size_t n = 0U;
    while (n < max_len && value[n] != '\0') {
        n++;
    }
    return n;
}

static bool lottery_finish_value_valid(const char *value)
{
    if (value == NULL) {
        return false;
    }
    return bounded_strnlen_local(value, ORB_CONFIG_PATH_MAX) < ORB_CONFIG_PATH_MAX;
}

static bool lottery_team_count_valid(uint8_t team_count)
{
    return team_count >= 2U && team_count <= ORB_LOTTERY_MAX_TEAMS;
}

static bool lottery_participants_total_valid(uint16_t participants_total)
{
    return participants_total >= 1U && participants_total <= ORB_LOTTERY_PARTICIPANTS_MAX;
}

static bool lottery_team_item_valid(const orb_lottery_team_config_t *team)
{
    if (team == NULL) {
        return false;
    }
    if (!lottery_team_source_valid(team->source)) {
        return false;
    }
    if (team->track_path[sizeof(team->track_path) - 1U] != '\0') {
        return false;
    }
    if (team->tts_text[sizeof(team->tts_text) - 1U] != '\0') {
        return false;
    }
    return true;
}

static bool lottery_settings_valid(uint8_t team_count,
                                   uint16_t participants_total,
                                   const orb_lottery_team_config_t *teams,
                                   size_t teams_count,
                                   uint8_t finish_source,
                                   const char *finish_value)
{
    if (!lottery_team_count_valid(team_count) || !lottery_participants_total_valid(participants_total)) {
        return false;
    }
    if (teams == NULL || teams_count != ORB_LOTTERY_MAX_TEAMS) {
        return false;
    }
    if (!lottery_team_source_valid(finish_source) || !lottery_finish_value_valid(finish_value)) {
        return false;
    }
    for (size_t i = 0U; i < teams_count; ++i) {
        if (!lottery_team_item_valid(&teams[i])) {
            return false;
        }
    }
    return true;
}

static bool wifi_sta_credentials_valid(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || password == NULL) {
        return false;
    }
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);
    return ssid_len < ORB_WIFI_SSID_MAX && pass_len < ORB_WIFI_PASSWORD_MAX;
}

static bool is_non_empty_text(const char *text)
{
    return text != NULL && text[0] != '\0';
}

static esp_err_t copy_path_checked(const char *src, char *dst, size_t dst_len)
{
    if (!is_non_empty_text(src) || dst == NULL || dst_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t src_len = strlen(src);
    if (src_len >= dst_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, src, src_len + 1U);
    return ESP_OK;
}

const char *config_manager_offline_submode_to_str(orb_offline_submode_t submode)
{
    switch (submode) {
    case ORB_OFFLINE_SUBMODE_AURA:
        return "aura";
    case ORB_OFFLINE_SUBMODE_LOTTERY:
        return "lottery";
    case ORB_OFFLINE_SUBMODE_PROPHECY:
        return "prophecy";
    default:
        return "unknown";
    }
}

bool config_manager_parse_offline_submode(const char *text, orb_offline_submode_t *out_submode)
{
    if (text == NULL || out_submode == NULL) {
        return false;
    }
    if (strcmp(text, "aura") == 0) {
        *out_submode = ORB_OFFLINE_SUBMODE_AURA;
        return true;
    }
    if (strcmp(text, "lottery") == 0) {
        *out_submode = ORB_OFFLINE_SUBMODE_LOTTERY;
        return true;
    }
    if (strcmp(text, "prophecy") == 0) {
        *out_submode = ORB_OFFLINE_SUBMODE_PROPHECY;
        return true;
    }
    return false;
}

esp_err_t config_manager_init(void)
{
    if (s_cfg_mutex == NULL) {
        s_cfg_mutex = xSemaphoreCreateMutex();
        if (s_cfg_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    config_defaults_load(&s_snapshot);

    bool loaded_from_nvs = false;
    esp_err_t load_err = config_store_nvs_load(&s_snapshot, &loaded_from_nvs);
    if (load_err == ESP_ERR_INVALID_VERSION) {
        ESP_LOGW(TAG, "NVS config version mismatch, writing defaults");
        (void)save_snapshot_unsafe();
        load_err = ESP_OK;
        loaded_from_nvs = false;
    }
    if (load_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS load failed, using defaults: %s", esp_err_to_name(load_err));
    }
    if (!hybrid_effect_scene_valid(s_snapshot.hybrid_effect_idle_scene_id)) {
        s_snapshot.hybrid_effect_idle_scene_id = ORB_LED_SCENE_ID_HYBRID_IDLE_SLOW_BREATHE;
    }
    if (!hybrid_effect_scene_valid(s_snapshot.hybrid_effect_talk_scene_id)) {
        s_snapshot.hybrid_effect_talk_scene_id = ORB_LED_SCENE_ID_HYBRID_VORTEX;
    }
    if (!hybrid_effect_palette_mode_valid(s_snapshot.hybrid_effect_palette_mode)) {
        s_snapshot.hybrid_effect_palette_mode = (uint8_t)ORB_LED_PALETTE_MODE_RAINBOW;
    }
    if (!lottery_team_count_valid(s_snapshot.offline_lottery_team_count)) {
        s_snapshot.offline_lottery_team_count = 4U;
    }
    if (!lottery_participants_total_valid(s_snapshot.offline_lottery_participants_total)) {
        s_snapshot.offline_lottery_participants_total = 17U;
    }
    for (size_t i = 0U; i < ORB_LOTTERY_MAX_TEAMS; ++i) {
        if (!lottery_team_source_valid(s_snapshot.offline_lottery_teams[i].source)) {
            s_snapshot.offline_lottery_teams[i].source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK;
        }
        s_snapshot.offline_lottery_teams[i].track_path[sizeof(s_snapshot.offline_lottery_teams[i].track_path) - 1U] = '\0';
        s_snapshot.offline_lottery_teams[i].tts_text[sizeof(s_snapshot.offline_lottery_teams[i].tts_text) - 1U] = '\0';
    }
    if (!lottery_team_source_valid(s_snapshot.offline_lottery_finish_source)) {
        s_snapshot.offline_lottery_finish_source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK;
    }
    s_snapshot.offline_lottery_finish_value[sizeof(s_snapshot.offline_lottery_finish_value) - 1U] = '\0';

    ESP_LOGI(TAG,
             "runtime config source=%s vol=%u bri=%u submode=%s net=%d mqtt=%d ai=%d web=%d "
             "aura_gap=%" PRIu32 " hybrid_mic_ms=%" PRIu32 " fx_idle=%" PRIu32 " fx_talk=%" PRIu32,
             loaded_from_nvs ? "nvs" : "defaults",
             (unsigned)s_snapshot.audio_volume,
             (unsigned)s_snapshot.led_brightness,
             config_manager_offline_submode_to_str(s_snapshot.offline_submode),
             s_snapshot.network_enabled,
             s_snapshot.mqtt_enabled,
             s_snapshot.ai_enabled,
             s_snapshot.web_enabled,
             s_snapshot.aura_gap_ms,
             s_snapshot.hybrid_mic_capture_ms,
             s_snapshot.hybrid_effect_idle_scene_id,
             s_snapshot.hybrid_effect_talk_scene_id);
    return ESP_OK;
}

esp_err_t config_manager_get_snapshot(orb_runtime_config_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    *snapshot = s_snapshot;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_get_led_brightness(uint8_t *brightness)
{
    if (brightness == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    *brightness = s_snapshot.led_brightness;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_get_aura_gap_ms(uint32_t *gap_ms)
{
    if (gap_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    *gap_ms = s_snapshot.aura_gap_ms;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_get_offline_submode(orb_offline_submode_t *submode)
{
    if (submode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    *submode = s_snapshot.offline_submode;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_get_offline_lottery_start_seq(uint32_t *start_seq)
{
    if (start_seq == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    *start_seq = s_snapshot.offline_lottery_start_seq;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_get_offline_lottery_params(uint8_t *team_count, uint16_t *participants_total)
{
    if (team_count == NULL || participants_total == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    *team_count = s_snapshot.offline_lottery_team_count;
    *participants_total = s_snapshot.offline_lottery_participants_total;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_get_offline_lottery_team(uint8_t index, orb_lottery_team_config_t *team)
{
    if (team == NULL || index >= ORB_LOTTERY_MAX_TEAMS) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    *team = s_snapshot.offline_lottery_teams[index];
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_get_offline_lottery_finish(uint8_t *source, char *value, size_t value_len)
{
    if (source == NULL || value == NULL || value_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    *source = s_snapshot.offline_lottery_finish_source;
    size_t src_len = bounded_strnlen_local(s_snapshot.offline_lottery_finish_value, sizeof(s_snapshot.offline_lottery_finish_value));
    size_t copy_len = (src_len < (value_len - 1U)) ? src_len : (value_len - 1U);
    if (copy_len > 0U) {
        memcpy(value, s_snapshot.offline_lottery_finish_value, copy_len);
    }
    value[copy_len] = '\0';
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_get_hybrid_effect_idle_scene_id(uint32_t *scene_id)
{
    if (scene_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    *scene_id = s_snapshot.hybrid_effect_idle_scene_id;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_set_feature_flags(bool network_enabled, bool mqtt_enabled, bool ai_enabled, bool web_enabled)
{
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.network_enabled = network_enabled;
    s_snapshot.mqtt_enabled = mqtt_enabled;
    s_snapshot.ai_enabled = ai_enabled;
    s_snapshot.web_enabled = web_enabled;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_led_brightness(uint8_t brightness)
{
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.led_brightness = brightness;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_audio_volume(uint8_t volume)
{
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.audio_volume = volume;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_offline_submode(orb_offline_submode_t submode)
{
    if (!offline_submode_valid(submode)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.offline_submode = submode;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_request_offline_lottery_start(void)
{
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.offline_lottery_start_seq++;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_set_offline_lottery_settings(uint8_t team_count,
                                                      uint16_t participants_total,
                                                      const orb_lottery_team_config_t *teams,
                                                      size_t teams_count,
                                                      uint8_t finish_source,
                                                      const char *finish_value)
{
    if (!lottery_settings_valid(team_count, participants_total, teams, teams_count, finish_source, finish_value)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.offline_lottery_team_count = team_count;
    s_snapshot.offline_lottery_participants_total = participants_total;
    for (size_t i = 0U; i < ORB_LOTTERY_MAX_TEAMS; ++i) {
        s_snapshot.offline_lottery_teams[i] = teams[i];
        s_snapshot.offline_lottery_teams[i].track_path[sizeof(s_snapshot.offline_lottery_teams[i].track_path) - 1U] = '\0';
        s_snapshot.offline_lottery_teams[i].tts_text[sizeof(s_snapshot.offline_lottery_teams[i].tts_text) - 1U] = '\0';
    }
    s_snapshot.offline_lottery_finish_source = finish_source;
    size_t finish_len = bounded_strnlen_local(finish_value, ORB_CONFIG_PATH_MAX - 1U);
    if (finish_len > 0U) {
        memcpy(s_snapshot.offline_lottery_finish_value, finish_value, finish_len);
    }
    s_snapshot.offline_lottery_finish_value[finish_len] = '\0';
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_aura_gap_ms(uint32_t gap_ms)
{
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.aura_gap_ms = gap_ms;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_aura_directories(const char *intro_dir, const char *response_dir)
{
    if (!is_non_empty_text(intro_dir) || !is_non_empty_text(response_dir)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }

    char intro_tmp[ORB_CONFIG_PATH_MAX] = { 0 };
    char response_tmp[ORB_CONFIG_PATH_MAX] = { 0 };
    err = copy_path_checked(intro_dir, intro_tmp, sizeof(intro_tmp));
    if (err == ESP_OK) {
        err = copy_path_checked(response_dir, response_tmp, sizeof(response_tmp));
    }
    if (err == ESP_OK) {
        memcpy(s_snapshot.aura_intro_dir, intro_tmp, sizeof(s_snapshot.aura_intro_dir));
        memcpy(s_snapshot.aura_response_dir, response_tmp, sizeof(s_snapshot.aura_response_dir));
        err = save_snapshot_unsafe();
    }

    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_aura_selected_color(const char *color_name)
{
    if (color_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(color_name);
    if (len >= sizeof(s_snapshot.aura_selected_color)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }

    memcpy(s_snapshot.aura_selected_color, color_name, len + 1U);
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_set_prophecy_timing(uint32_t gap12_ms, uint32_t gap23_ms, uint32_t gap34_ms, uint32_t leadin_wait_ms)
{
    if (!prophecy_timing_valid(gap12_ms, gap23_ms, gap34_ms, leadin_wait_ms)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.prophecy_gap12_ms = gap12_ms;
    s_snapshot.prophecy_gap23_ms = gap23_ms;
    s_snapshot.prophecy_gap34_ms = gap34_ms;
    s_snapshot.prophecy_leadin_wait_ms = leadin_wait_ms;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_prophecy_background(uint16_t gain_permille, uint32_t fade_in_ms, uint32_t fade_out_ms)
{
    if (!prophecy_background_valid(gain_permille, fade_in_ms, fade_out_ms)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.prophecy_bg_gain_permille = gain_permille;
    s_snapshot.prophecy_bg_fade_in_ms = fade_in_ms;
    s_snapshot.prophecy_bg_fade_out_ms = fade_out_ms;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_hybrid_params(uint16_t reject_threshold_permille, uint32_t mic_capture_ms)
{
    if (!hybrid_params_valid(reject_threshold_permille, mic_capture_ms)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.hybrid_reject_threshold_permille = reject_threshold_permille;
    s_snapshot.hybrid_mic_capture_ms = mic_capture_ms;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_hybrid_unknown_retry_max(uint8_t unknown_retry_max)
{
    if (!hybrid_unknown_retry_valid(unknown_retry_max)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.hybrid_unknown_retry_max = unknown_retry_max;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_hybrid_effect(uint32_t idle_scene_id,
                                           uint32_t talk_scene_id,
                                           uint8_t speed,
                                           uint8_t intensity,
                                           uint8_t scale)
{
    if (!hybrid_effect_valid(idle_scene_id, talk_scene_id, speed, intensity, scale)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.hybrid_effect_idle_scene_id = idle_scene_id;
    s_snapshot.hybrid_effect_talk_scene_id = talk_scene_id;
    s_snapshot.hybrid_effect_speed = speed;
    s_snapshot.hybrid_effect_intensity = intensity;
    s_snapshot.hybrid_effect_scale = scale;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_hybrid_effect_palette(uint8_t palette_mode,
                                                   uint8_t color1_r,
                                                   uint8_t color1_g,
                                                   uint8_t color1_b,
                                                   uint8_t color2_r,
                                                   uint8_t color2_g,
                                                   uint8_t color2_b,
                                                   uint8_t color3_r,
                                                   uint8_t color3_g,
                                                   uint8_t color3_b)
{
    if (!hybrid_effect_palette_mode_valid(palette_mode)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    s_snapshot.hybrid_effect_palette_mode = palette_mode;
    s_snapshot.hybrid_effect_color1_r = color1_r;
    s_snapshot.hybrid_effect_color1_g = color1_g;
    s_snapshot.hybrid_effect_color1_b = color1_b;
    s_snapshot.hybrid_effect_color2_r = color2_r;
    s_snapshot.hybrid_effect_color2_g = color2_g;
    s_snapshot.hybrid_effect_color2_b = color2_b;
    s_snapshot.hybrid_effect_color3_r = color3_r;
    s_snapshot.hybrid_effect_color3_g = color3_g;
    s_snapshot.hybrid_effect_color3_b = color3_b;
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_set_wifi_sta_credentials(const char *ssid, const char *password)
{
    if (!wifi_sta_credentials_valid(ssid, password)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }

    (void)memset(s_snapshot.wifi_sta_ssid, 0, sizeof(s_snapshot.wifi_sta_ssid));
    (void)memset(s_snapshot.wifi_sta_password, 0, sizeof(s_snapshot.wifi_sta_password));
    (void)memcpy(s_snapshot.wifi_sta_ssid, ssid, strlen(ssid));
    (void)memcpy(s_snapshot.wifi_sta_password, password, strlen(password));

    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_get_wifi_sta_credentials(char *ssid,
                                                  size_t ssid_len,
                                                  char *password,
                                                  size_t pass_len)
{
    if ((ssid == NULL && password == NULL) || (ssid != NULL && ssid_len == 0U) || (password != NULL && pass_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }

    if (ssid != NULL) {
        strlcpy(ssid, s_snapshot.wifi_sta_ssid, ssid_len);
    }
    if (password != NULL) {
        strlcpy(password, s_snapshot.wifi_sta_password, pass_len);
    }

    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t config_manager_has_persisted_wifi_sta_credentials(bool *has_credentials)
{
    return config_store_nvs_has_wifi_sta_credentials(has_credentials);
}

esp_err_t config_manager_save(void)
{
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }
    err = save_snapshot_unsafe();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_reload(void)
{
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }

    config_defaults_load(&s_snapshot);
    bool loaded = false;
    err = config_store_nvs_load(&s_snapshot, &loaded);
    if (err == ESP_ERR_INVALID_VERSION) {
        (void)save_snapshot_unsafe();
        err = ESP_OK;
    }
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t config_manager_reset_to_defaults(bool persist)
{
    esp_err_t err = lock_cfg();
    if (err != ESP_OK) {
        return err;
    }

    config_defaults_load(&s_snapshot);
    if (persist) {
        err = save_snapshot_unsafe();
    } else {
        err = ESP_OK;
    }
    xSemaphoreGive(s_cfg_mutex);
    return err;
}
