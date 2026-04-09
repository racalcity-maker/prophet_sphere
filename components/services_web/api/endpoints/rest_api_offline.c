#include "rest_api_modules.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "app_tasking.h"
#include "config_manager.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "log_tags.h"
#include "mode_manager.h"
#include "rest_api_common.h"
#include "sdkconfig.h"
#include "session_controller.h"

static const char *TAG = LOG_TAG_REST;

static uint32_t request_timeout_ms(void)
{
    return (uint32_t)CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
}

#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_WAV_PATH
#define CONFIG_ORB_AUDIO_PROPHECY_BG_WAV_PATH "/sdcard/audio/backgrounds/oracle.wav"
#endif

#define OFFLINE_FORM_MAX 4096
#define OFFLINE_PARAM_ENCODED_MAX (ORB_CONFIG_PATH_MAX * 4U)
/* Large form scratch is static to keep HTTP handler stack usage predictable. */
static char s_offline_form_body[OFFLINE_FORM_MAX + 1U];
static int offline_hex_nibble(char c);
static bool offline_url_decode_inplace(char *text);
static bool offline_copy_text_checked(char *dst, size_t dst_len, const char *src);

static esp_err_t offline_read_form_body(httpd_req_t *req, char *out, size_t out_len)
{
    if (req == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (req->content_len <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t)req->content_len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    int remaining = req->content_len;
    int total = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, out + total, remaining);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        total += n;
        remaining -= n;
    }
    out[total] = '\0';
    return ESP_OK;
}

static esp_err_t offline_get_param(httpd_req_t *req, const char *form_body, const char *key, char *out, size_t out_len)
{
    if (req == NULL || key == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t qerr = rest_api_query_value(req, key, out, out_len);
    if (qerr == ESP_OK) {
        if (!offline_url_decode_inplace(out)) {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }
    if (form_body != NULL && form_body[0] != '\0') {
        esp_err_t ferr = httpd_query_key_value(form_body, key, out, out_len);
        if (ferr == ESP_OK) {
            if (!offline_url_decode_inplace(out)) {
                return ESP_ERR_INVALID_ARG;
            }
            return ESP_OK;
        }
        if (ferr == ESP_ERR_HTTPD_RESULT_TRUNC) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static int offline_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (int)(c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (int)(c - 'A');
    }
    return -1;
}

static bool offline_url_decode_inplace(char *text)
{
    if (text == NULL) {
        return false;
    }
    char *src = text;
    char *dst = text;
    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
            int hi = offline_hex_nibble(src[1]);
            int lo = offline_hex_nibble(src[2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    return true;
}

static bool offline_copy_text_checked(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || src == NULL || dst_len == 0U) {
        return false;
    }
    dst[0] = '\0';
    size_t src_len = strlen(src);
    if (src_len >= dst_len) {
        return false;
    }
    memcpy(dst, src, src_len + 1U);
    return true;
}

static const char *lottery_source_to_str(uint8_t source)
{
    return (source == (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TTS) ? "tts" : "track";
}

static bool parse_lottery_source(const char *text, uint8_t *out_source)
{
    if (text == NULL || out_source == NULL) {
        return false;
    }
    if (strcmp(text, "tts") == 0) {
        *out_source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TTS;
        return true;
    }
    if (strcmp(text, "track") == 0) {
        *out_source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK;
        return true;
    }
    return false;
}

static esp_err_t send_offline_state(httpd_req_t *req)
{
    orb_runtime_config_t cfg = { 0 };
    session_info_t session = { 0 };
    (void)config_manager_get_snapshot(&cfg);
    (void)session_controller_get_info(&session);

    esp_err_t buf_err = rest_api_web_buffer_lock();
    if (buf_err != ESP_OK) {
        return rest_api_send_error_json(req, "500 Internal Server Error", "offline_buf_lock_failed");
    }
    size_t json_cap = 0U;
    char *json = rest_api_shared_json_buffer(&json_cap);
    if (json == NULL || json_cap == 0U) {
        rest_api_web_buffer_unlock();
        return rest_api_send_error_json(req, "500 Internal Server Error", "offline_no_mem");
    }
    json[0] = '\0';
    char finish_value[ORB_CONFIG_PATH_MAX];
    (void)offline_copy_text_checked(finish_value, sizeof(finish_value), cfg.offline_lottery_finish_value);
    (void)offline_url_decode_inplace(finish_value);

    int n = snprintf(
        json,
        json_cap,
        "{\"ok\":true,"
        "\"mode\":\"%s\","
        "\"submode\":\"%s\","
        "\"session\":{\"active\":%s,\"state\":\"%s\",\"id\":%" PRIu32 "},"
        "\"audio\":{\"fg_volume\":%u,\"bg_path\":\"%s\",\"bg_gain_permille\":%u,"
        "\"bg_fade_in_ms\":%u,\"bg_fade_out_ms\":%u},"
        "\"prophecy\":{\"gap12_ms\":%u,\"gap23_ms\":%u,\"gap34_ms\":%u,\"leadin_wait_ms\":%u},"
        "\"aura\":{\"gap_ms\":%" PRIu32 ",\"intro_dir\":\"%s\",\"response_dir\":\"%s\",\"selected_color\":\"%s\"},"
        "\"lottery\":{\"start_seq\":%" PRIu32 ",\"team_count\":%u,\"participants_total\":%u,"
        "\"finish_source\":\"%s\",\"finish_value\":\"%s\",\"teams\":[",
        mode_manager_mode_to_str(mode_manager_get_current_mode()),
        config_manager_offline_submode_to_str(cfg.offline_submode),
        session.active ? "true" : "false",
        session_controller_state_to_str(session.state),
        session.session_id,
        cfg.audio_volume,
        CONFIG_ORB_AUDIO_PROPHECY_BG_WAV_PATH,
        (unsigned)cfg.prophecy_bg_gain_permille,
        (unsigned)cfg.prophecy_bg_fade_in_ms,
        (unsigned)cfg.prophecy_bg_fade_out_ms,
        (unsigned)cfg.prophecy_gap12_ms,
        (unsigned)cfg.prophecy_gap23_ms,
        (unsigned)cfg.prophecy_gap34_ms,
        (unsigned)cfg.prophecy_leadin_wait_ms,
        cfg.aura_gap_ms,
        cfg.aura_intro_dir,
        cfg.aura_response_dir,
        cfg.aura_selected_color,
        cfg.offline_lottery_start_seq,
        (unsigned)cfg.offline_lottery_team_count,
        (unsigned)cfg.offline_lottery_participants_total,
        lottery_source_to_str(cfg.offline_lottery_finish_source),
        finish_value);
    if (n <= 0 || (size_t)n >= json_cap) {
        rest_api_web_buffer_unlock();
        return rest_api_send_error_json(req, "500 Internal Server Error", "offline_state_serialize_failed");
    }
    size_t used = (size_t)n;

    for (size_t i = 0U; i < ORB_LOTTERY_MAX_TEAMS; ++i) {
        const orb_lottery_team_config_t *team = &cfg.offline_lottery_teams[i];
        char track_path[ORB_CONFIG_PATH_MAX];
        char tts_text[ORB_CONFIG_PATH_MAX];
        (void)snprintf(track_path, sizeof(track_path), "%s", team->track_path);
        (void)snprintf(tts_text, sizeof(tts_text), "%s", team->tts_text);
        (void)offline_url_decode_inplace(track_path);
        (void)offline_url_decode_inplace(tts_text);
        n = snprintf(json + used,
                     json_cap - used,
                     "%s{\"index\":%u,\"color_r\":%u,\"color_g\":%u,\"color_b\":%u,"
                     "\"source\":\"%s\",\"track_path\":\"%s\",\"tts_text\":\"%s\"}",
                     (i == 0U) ? "" : ",",
                     (unsigned)(i + 1U),
                     (unsigned)team->color_r,
                     (unsigned)team->color_g,
                     (unsigned)team->color_b,
                     lottery_source_to_str(team->source),
                     track_path,
                     tts_text);
        if (n <= 0 || (size_t)n >= (json_cap - used)) {
            rest_api_web_buffer_unlock();
            return rest_api_send_error_json(req, "500 Internal Server Error", "offline_state_serialize_failed");
        }
        used += (size_t)n;
    }

    n = snprintf(json + used, json_cap - used, "]}}");
    if (n <= 0 || (size_t)n >= (json_cap - used)) {
        rest_api_web_buffer_unlock();
        return rest_api_send_error_json(req, "500 Internal Server Error", "offline_state_serialize_failed");
    }
    esp_err_t send_err = rest_api_send_json(req, "200 OK", json);
    rest_api_web_buffer_unlock();
    return send_err;
}

static bool ensure_offline_mode(void)
{
    return mode_manager_get_current_mode() == ORB_MODE_OFFLINE_SCRIPTED;
}

static esp_err_t offline_state_handler(httpd_req_t *req)
{
    return send_offline_state(req);
}

static esp_err_t offline_config_get_handler(httpd_req_t *req)
{
    return send_offline_state(req);
}

static esp_err_t offline_config_post_handler(httpd_req_t *req)
{
    if (!ensure_offline_mode()) {
        return rest_api_send_error_json(req, "409 Conflict", "offline_config_requires_offline_mode");
    }

    orb_runtime_config_t cfg = { 0 };
    if (config_manager_get_snapshot(&cfg) != ESP_OK) {
        return rest_api_send_error_json(req, "500 Internal Server Error", "config_snapshot_failed");
    }
    s_offline_form_body[0] = '\0';
    (void)offline_read_form_body(req, s_offline_form_body, sizeof(s_offline_form_body));

    bool has_any = false;
    char value[ORB_CONFIG_PATH_MAX];
    char value_encoded[OFFLINE_PARAM_ENCODED_MAX];

    if (rest_api_query_value(req, "submode", value, sizeof(value)) == ESP_OK ||
        rest_api_query_value(req, "offline_submode", value, sizeof(value)) == ESP_OK) {
        orb_offline_submode_t submode = ORB_OFFLINE_SUBMODE_AURA;
        if (!config_manager_parse_offline_submode(value, &submode)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_submode");
        }
        (void)config_manager_set_offline_submode(submode);
        has_any = true;
    }

    if (rest_api_query_value(req, "fg_volume", value, sizeof(value)) == ESP_OK ||
        rest_api_query_value(req, "volume", value, sizeof(value)) == ESP_OK) {
        uint32_t volume = 0U;
        if (!rest_api_parse_u32(value, &volume) || volume > 100U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_fg_volume");
        }
        (void)config_manager_set_audio_volume((uint8_t)volume);
        audio_command_t audio_cmd = { 0 };
        audio_cmd.id = AUDIO_CMD_SET_VOLUME;
        audio_cmd.payload.set_volume.volume = (uint8_t)volume;
        (void)app_tasking_send_audio_command(&audio_cmd, request_timeout_ms());
        has_any = true;
    }

    if (rest_api_query_value(req, "aura_gap_ms", value, sizeof(value)) == ESP_OK) {
        uint32_t gap_ms = 0U;
        if (!rest_api_parse_u32(value, &gap_ms) || gap_ms > 60000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_aura_gap_ms");
        }
        (void)config_manager_set_aura_gap_ms(gap_ms);
        has_any = true;
    }

    char intro_dir[ORB_CONFIG_PATH_MAX] = { 0 };
    char response_dir[ORB_CONFIG_PATH_MAX] = { 0 };
    bool intro_changed = (rest_api_query_value(req, "aura_intro_dir", intro_dir, sizeof(intro_dir)) == ESP_OK);
    bool response_changed = (rest_api_query_value(req, "aura_response_dir", response_dir, sizeof(response_dir)) == ESP_OK);
    if (intro_changed || response_changed) {
        const char *intro = intro_changed ? intro_dir : cfg.aura_intro_dir;
        const char *response = response_changed ? response_dir : cfg.aura_response_dir;
        if (config_manager_set_aura_directories(intro, response) != ESP_OK) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_aura_dirs");
        }
        has_any = true;
    }

    bool prophecy_timing_changed = false;
    uint32_t gap12_ms = cfg.prophecy_gap12_ms;
    uint32_t gap23_ms = cfg.prophecy_gap23_ms;
    uint32_t gap34_ms = cfg.prophecy_gap34_ms;
    uint32_t leadin_wait_ms = cfg.prophecy_leadin_wait_ms;
    if (rest_api_query_value(req, "prophecy_gap12_ms", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &gap12_ms) || gap12_ms > 60000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_gap12_ms");
        }
        prophecy_timing_changed = true;
    }
    if (rest_api_query_value(req, "prophecy_gap23_ms", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &gap23_ms) || gap23_ms > 60000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_gap23_ms");
        }
        prophecy_timing_changed = true;
    }
    if (rest_api_query_value(req, "prophecy_gap34_ms", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &gap34_ms) || gap34_ms > 60000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_gap34_ms");
        }
        prophecy_timing_changed = true;
    }
    if (rest_api_query_value(req, "prophecy_leadin_wait_ms", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &leadin_wait_ms) || leadin_wait_ms > 60000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_leadin_wait_ms");
        }
        prophecy_timing_changed = true;
    }
    if (prophecy_timing_changed) {
        if (config_manager_set_prophecy_timing(gap12_ms, gap23_ms, gap34_ms, leadin_wait_ms) != ESP_OK) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_timing");
        }
        cfg.prophecy_gap12_ms = gap12_ms;
        cfg.prophecy_gap23_ms = gap23_ms;
        cfg.prophecy_gap34_ms = gap34_ms;
        cfg.prophecy_leadin_wait_ms = leadin_wait_ms;
        has_any = true;
    }

    bool prophecy_bg_changed = false;
    uint32_t bg_gain_u32 = cfg.prophecy_bg_gain_permille;
    uint32_t bg_fade_in_ms = cfg.prophecy_bg_fade_in_ms;
    uint32_t bg_fade_out_ms = cfg.prophecy_bg_fade_out_ms;
    if (rest_api_query_value(req, "prophecy_bg_gain_permille", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &bg_gain_u32) || bg_gain_u32 > 2000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_bg_gain");
        }
        prophecy_bg_changed = true;
    }
    if (rest_api_query_value(req, "prophecy_bg_fade_in_ms", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &bg_fade_in_ms) || bg_fade_in_ms > 60000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_bg_fade_in_ms");
        }
        prophecy_bg_changed = true;
    }
    if (rest_api_query_value(req, "prophecy_bg_fade_out_ms", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &bg_fade_out_ms) || bg_fade_out_ms > 60000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_bg_fade_out_ms");
        }
        prophecy_bg_changed = true;
    }
    if (prophecy_bg_changed) {
        if (config_manager_set_prophecy_background((uint16_t)bg_gain_u32, bg_fade_in_ms, bg_fade_out_ms) != ESP_OK) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_background");
        }
        cfg.prophecy_bg_gain_permille = (uint16_t)bg_gain_u32;
        cfg.prophecy_bg_fade_in_ms = bg_fade_in_ms;
        cfg.prophecy_bg_fade_out_ms = bg_fade_out_ms;
        has_any = true;
    }

    uint8_t lottery_team_count = cfg.offline_lottery_team_count;
    uint16_t lottery_participants_total = cfg.offline_lottery_participants_total;
    uint8_t lottery_finish_source = cfg.offline_lottery_finish_source;
    char lottery_finish_value[ORB_CONFIG_PATH_MAX];
    (void)offline_copy_text_checked(lottery_finish_value, sizeof(lottery_finish_value), cfg.offline_lottery_finish_value);
    bool lottery_finish_source_set = false;
    bool lottery_finish_value_set = false;
    bool lottery_changed = false;
    bool lottery_force_source = false;
    uint8_t lottery_forced_source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK;

    if (offline_get_param(req, s_offline_form_body, "lottery_team_count", value, sizeof(value)) == ESP_OK) {
        uint32_t v = 0U;
        if (!rest_api_parse_u32(value, &v) || v < 2U || v > ORB_LOTTERY_MAX_TEAMS) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_team_count");
        }
        lottery_team_count = (uint8_t)v;
        lottery_changed = true;
    }
    if (offline_get_param(req, s_offline_form_body, "lottery_participants_total", value, sizeof(value)) == ESP_OK) {
        uint32_t v = 0U;
        if (!rest_api_parse_u32(value, &v) || v < 1U || v > ORB_LOTTERY_PARTICIPANTS_MAX) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_participants_total");
        }
        lottery_participants_total = (uint16_t)v;
        lottery_changed = true;
    }
    if (offline_get_param(req, s_offline_form_body, "lottery_mode", value, sizeof(value)) == ESP_OK) {
        if (strcmp(value, "tts") == 0) {
            lottery_forced_source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TTS;
        } else if (strcmp(value, "track") == 0) {
            lottery_forced_source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK;
        } else {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_mode");
        }
        lottery_force_source = true;
        lottery_changed = true;
    }
    if (offline_get_param(req, s_offline_form_body, "lottery_finish_source", value, sizeof(value)) == ESP_OK) {
        if (!parse_lottery_source(value, &lottery_finish_source)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_finish_source");
        }
        lottery_finish_source_set = true;
        lottery_changed = true;
    }
    esp_err_t finish_value_err =
        offline_get_param(req, s_offline_form_body, "lottery_finish_value", value_encoded, sizeof(value_encoded));
    if (finish_value_err == ESP_ERR_INVALID_SIZE) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_finish_value");
    }
    if (finish_value_err == ESP_OK) {
        if (!offline_copy_text_checked(lottery_finish_value, sizeof(lottery_finish_value), value_encoded)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_finish_value");
        }
        lottery_finish_value_set = true;
        lottery_changed = true;
    }

    for (size_t i = 0U; i < ORB_LOTTERY_MAX_TEAMS; ++i) {
        char key[40] = { 0 };
        uint32_t v = 0U;

        (void)snprintf(key, sizeof(key), "lottery_t%u_r", (unsigned)(i + 1U));
        if (offline_get_param(req, s_offline_form_body, key, value, sizeof(value)) == ESP_OK) {
            if (!rest_api_parse_u32(value, &v) || v > 255U) {
                return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_team_color_r");
            }
            cfg.offline_lottery_teams[i].color_r = (uint8_t)v;
            lottery_changed = true;
        }

        (void)snprintf(key, sizeof(key), "lottery_t%u_g", (unsigned)(i + 1U));
        if (offline_get_param(req, s_offline_form_body, key, value, sizeof(value)) == ESP_OK) {
            if (!rest_api_parse_u32(value, &v) || v > 255U) {
                return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_team_color_g");
            }
            cfg.offline_lottery_teams[i].color_g = (uint8_t)v;
            lottery_changed = true;
        }

        (void)snprintf(key, sizeof(key), "lottery_t%u_b", (unsigned)(i + 1U));
        if (offline_get_param(req, s_offline_form_body, key, value, sizeof(value)) == ESP_OK) {
            if (!rest_api_parse_u32(value, &v) || v > 255U) {
                return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_team_color_b");
            }
            cfg.offline_lottery_teams[i].color_b = (uint8_t)v;
            lottery_changed = true;
        }

        (void)snprintf(key, sizeof(key), "lottery_t%u_source", (unsigned)(i + 1U));
        if (offline_get_param(req, s_offline_form_body, key, value, sizeof(value)) == ESP_OK) {
            uint8_t source = 0U;
            if (!parse_lottery_source(value, &source)) {
                return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_team_source");
            }
            cfg.offline_lottery_teams[i].source = source;
            lottery_changed = true;
        }

        (void)snprintf(key, sizeof(key), "lottery_t%u_track", (unsigned)(i + 1U));
        esp_err_t team_track_err = offline_get_param(req, s_offline_form_body, key, value_encoded, sizeof(value_encoded));
        if (team_track_err == ESP_ERR_INVALID_SIZE) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_team_track");
        }
        if (team_track_err == ESP_OK) {
            if (!offline_copy_text_checked(cfg.offline_lottery_teams[i].track_path,
                                           sizeof(cfg.offline_lottery_teams[i].track_path),
                                           value_encoded)) {
                return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_team_track");
            }
            lottery_changed = true;
        }

        (void)snprintf(key, sizeof(key), "lottery_t%u_tts", (unsigned)(i + 1U));
        esp_err_t team_tts_err = offline_get_param(req, s_offline_form_body, key, value_encoded, sizeof(value_encoded));
        if (team_tts_err == ESP_ERR_INVALID_SIZE) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_team_tts");
        }
        if (team_tts_err == ESP_OK) {
            if (!offline_copy_text_checked(cfg.offline_lottery_teams[i].tts_text,
                                           sizeof(cfg.offline_lottery_teams[i].tts_text),
                                           value_encoded)) {
                return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_team_tts");
            }
            lottery_changed = true;
        }
    }

    if (lottery_force_source) {
        for (size_t i = 0U; i < ORB_LOTTERY_MAX_TEAMS; ++i) {
            cfg.offline_lottery_teams[i].source = lottery_forced_source;
        }
    }

    if (lottery_finish_source == (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TTS && lottery_finish_value[0] == '\0') {
        if (cfg.offline_lottery_teams[0].tts_text[0] != '\0') {
            (void)offline_copy_text_checked(lottery_finish_value, sizeof(lottery_finish_value), cfg.offline_lottery_teams[0].tts_text);
        } else {
            (void)offline_copy_text_checked(lottery_finish_value, sizeof(lottery_finish_value), "Лотерея завершена.");
        }
        lottery_changed = true;
    }

    if (lottery_changed) {
        ESP_LOGI(TAG,
                 "offline.lottery save request: teams=%u total=%u finish_source=%s(set=%u) finish_value='%s'(set=%u)",
                 (unsigned)lottery_team_count,
                 (unsigned)lottery_participants_total,
                 lottery_source_to_str(lottery_finish_source),
                 (unsigned)(lottery_finish_source_set ? 1U : 0U),
                 lottery_finish_value,
                 (unsigned)(lottery_finish_value_set ? 1U : 0U));
        if (config_manager_set_offline_lottery_settings(
                lottery_team_count,
                lottery_participants_total,
                cfg.offline_lottery_teams,
                ORB_LOTTERY_MAX_TEAMS,
                lottery_finish_source,
                lottery_finish_value) != ESP_OK) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_lottery_settings");
        }
        cfg.offline_lottery_team_count = lottery_team_count;
        cfg.offline_lottery_participants_total = lottery_participants_total;
        cfg.offline_lottery_finish_source = lottery_finish_source;
        (void)offline_copy_text_checked(cfg.offline_lottery_finish_value,
                                        sizeof(cfg.offline_lottery_finish_value),
                                        lottery_finish_value);
        ESP_LOGI(TAG,
                 "offline.lottery saved: finish_source=%s finish_value='%s'",
                 lottery_source_to_str(cfg.offline_lottery_finish_source),
                 cfg.offline_lottery_finish_value);
        has_any = true;
    }

    bool persist = false;
    if (offline_get_param(req, s_offline_form_body, "save", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_bool_text(value, &persist)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_save");
        }
    }
    if (persist) {
        (void)config_manager_save();
    }

    if (!has_any) {
        return rest_api_send_error_json(req, "400 Bad Request", "no_valid_params");
    }

    return send_offline_state(req);
}

static esp_err_t offline_submode_handler(httpd_req_t *req)
{
    if (!ensure_offline_mode()) {
        return rest_api_send_error_json(req, "409 Conflict", "submode_switch_requires_offline_mode");
    }

    char value[32];
    if (rest_api_query_value(req, "name", value, sizeof(value)) != ESP_OK) {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_submode_name");
    }

    orb_offline_submode_t submode = ORB_OFFLINE_SUBMODE_AURA;
    if (!config_manager_parse_offline_submode(value, &submode)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_submode_name");
    }

    esp_err_t err = config_manager_set_offline_submode(submode);
    if (err != ESP_OK) {
        return rest_api_send_error_json(req, "500 Internal Server Error", "set_submode_failed");
    }

    return send_offline_state(req);
}

static esp_err_t offline_action_handler(httpd_req_t *req)
{
    if (!ensure_offline_mode()) {
        return rest_api_send_error_json(req, "409 Conflict", "offline_action_requires_offline_mode");
    }

    char value[48];
    if (rest_api_query_value(req, "name", value, sizeof(value)) != ESP_OK) {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_action_name");
    }

    if (strcmp(value, "lottery_start") == 0) {
        orb_offline_submode_t submode = ORB_OFFLINE_SUBMODE_AURA;
        if (config_manager_get_offline_submode(&submode) != ESP_OK) {
            return rest_api_send_error_json(req, "500 Internal Server Error", "offline_submode_read_failed");
        }
        if (submode != ORB_OFFLINE_SUBMODE_LOTTERY) {
            /* Keep lottery start resilient: auto-switch submode instead of rejecting. */
            if (config_manager_set_offline_submode(ORB_OFFLINE_SUBMODE_LOTTERY) != ESP_OK) {
                return rest_api_send_error_json(req, "500 Internal Server Error", "lottery_submode_switch_failed");
            }
        }
        if (config_manager_request_offline_lottery_start() != ESP_OK) {
            return rest_api_send_error_json(req, "500 Internal Server Error", "lottery_start_request_failed");
        }
        return rest_api_send_json(req, "200 OK", "{\"ok\":true,\"action\":\"lottery_start\"}");
    }

    if (strcmp(value, "audio_stop") == 0) {
        audio_command_t audio_cmd = { 0 };
        audio_cmd.id = AUDIO_CMD_STOP;
        if (app_tasking_send_audio_command(&audio_cmd, request_timeout_ms()) != ESP_OK) {
            return rest_api_send_error_json(req, "500 Internal Server Error", "audio_stop_failed");
        }
        return rest_api_send_json(req, "200 OK", "{\"ok\":true,\"action\":\"audio_stop\"}");
    }

    return rest_api_send_error_json(req, "400 Bad Request", "unknown_offline_action");
}

esp_err_t rest_api_register_offline_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t offline_state = {
        .uri = "/api/offline/state",
        .method = HTTP_GET,
        .handler = offline_state_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t offline_cfg_get = {
        .uri = "/api/offline/config",
        .method = HTTP_GET,
        .handler = offline_config_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t offline_cfg_post = {
        .uri = "/api/offline/config",
        .method = HTTP_POST,
        .handler = offline_config_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t offline_submode = {
        .uri = "/api/offline/submode",
        .method = HTTP_POST,
        .handler = offline_submode_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t offline_action = {
        .uri = "/api/offline/action",
        .method = HTTP_POST,
        .handler = offline_action_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &offline_state), TAG, "register offline state failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &offline_cfg_get), TAG, "register offline config GET failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &offline_cfg_post), TAG, "register offline config POST failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &offline_submode), TAG, "register offline submode failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &offline_action), TAG, "register offline action failed");
    return ESP_OK;
}
