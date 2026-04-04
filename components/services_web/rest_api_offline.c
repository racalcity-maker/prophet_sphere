#include "rest_api_modules.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "audio_service.h"
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
        "\"lottery\":{\"start_seq\":%" PRIu32 "}}",
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
        cfg.offline_lottery_start_seq);
    if (n <= 0 || (size_t)n >= json_cap) {
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

    bool has_any = false;
    char value[ORB_CONFIG_PATH_MAX];

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
        (void)audio_service_set_volume((uint8_t)volume, request_timeout_ms());
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

    bool persist = false;
    if (rest_api_query_value(req, "save", value, sizeof(value)) == ESP_OK) {
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
        if (config_manager_request_offline_lottery_start() != ESP_OK) {
            return rest_api_send_error_json(req, "500 Internal Server Error", "lottery_start_request_failed");
        }
        return rest_api_send_json(req, "200 OK", "{\"ok\":true,\"action\":\"lottery_start\"}");
    }

    if (strcmp(value, "audio_stop") == 0) {
        if (audio_service_stop(request_timeout_ms()) != ESP_OK) {
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
