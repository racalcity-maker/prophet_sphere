#include "rest_api_modules.h"

#include <inttypes.h>
#include <stdio.h>
#include "audio_service.h"
#include "config_manager.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "led_service.h"
#include "log_tags.h"
#include "rest_api_common.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_REST;

static uint32_t request_timeout_ms(void)
{
    return (uint32_t)CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
}

static esp_err_t send_config_snapshot(httpd_req_t *req)
{
    orb_runtime_config_t cfg = { 0 };
    esp_err_t err = config_manager_get_snapshot(&cfg);
    if (err != ESP_OK) {
        return rest_api_send_error_json(req, "500 Internal Server Error", "config_read_failed");
    }

    char json[1024];
    (void)snprintf(json,
                   sizeof(json),
                   "{\"ok\":true,\"brightness\":%u,\"volume\":%u,\"network\":%s,\"mqtt\":%s,\"ai\":%s,\"web\":%s,"
                   "\"offline_submode\":\"%s\",\"aura_gap_ms\":%" PRIu32 ","
                   "\"aura_intro_dir\":\"%s\",\"aura_response_dir\":\"%s\","
                   "\"hybrid_reject_threshold_permille\":%u,\"hybrid_mic_capture_ms\":%" PRIu32 ","
                   "\"hybrid_unknown_retry_max\":%u,\"prophecy_bg_fade_out_ms\":%" PRIu32 "}",
                   cfg.led_brightness,
                   cfg.audio_volume,
                   cfg.network_enabled ? "true" : "false",
                   cfg.mqtt_enabled ? "true" : "false",
                   cfg.ai_enabled ? "true" : "false",
                   cfg.web_enabled ? "true" : "false",
                   config_manager_offline_submode_to_str(cfg.offline_submode),
                   cfg.aura_gap_ms,
                   cfg.aura_intro_dir,
                   cfg.aura_response_dir,
                   (unsigned)cfg.hybrid_reject_threshold_permille,
                   cfg.hybrid_mic_capture_ms,
                   (unsigned)cfg.hybrid_unknown_retry_max,
                   cfg.prophecy_bg_fade_out_ms);
    return rest_api_send_json(req, "200 OK", json);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "config GET uri=%s", req ? req->uri : "-");
    return send_config_snapshot(req);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    orb_runtime_config_t cfg = { 0 };
    esp_err_t err = config_manager_get_snapshot(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config snapshot failed: %s", esp_err_to_name(err));
        return rest_api_send_error_json(req, "500 Internal Server Error", "config_read_failed");
    }

    bool has_any = false;
    char value[160];
    bool brightness_changed = false;
    bool volume_changed = false;
    bool submode_changed = false;
    bool aura_changed = false;
    bool hybrid_changed = false;
    bool hybrid_retry_changed = false;
    bool bg_fade_out_changed = false;
    bool flags_changed = false;

    if (rest_api_query_value(req, "brightness", value, sizeof(value)) == ESP_OK) {
        uint32_t brightness = 0;
        if (!rest_api_parse_u32(value, &brightness) || brightness > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_brightness");
        }
        (void)config_manager_set_led_brightness((uint8_t)brightness);
        (void)led_service_set_brightness((led_brightness_t)brightness, request_timeout_ms());
        cfg.led_brightness = (uint8_t)brightness;
        has_any = true;
        brightness_changed = true;
    }

    if (rest_api_query_value(req, "volume", value, sizeof(value)) == ESP_OK) {
        uint32_t volume = 0;
        if (!rest_api_parse_u32(value, &volume) || volume > 100U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_volume");
        }
        (void)config_manager_set_audio_volume((uint8_t)volume);
        (void)audio_service_set_volume((uint8_t)volume, request_timeout_ms());
        cfg.audio_volume = (uint8_t)volume;
        has_any = true;
        volume_changed = true;
    }

    if (rest_api_query_value(req, "offline_submode", value, sizeof(value)) == ESP_OK) {
        orb_offline_submode_t submode = ORB_OFFLINE_SUBMODE_AURA;
        if (!config_manager_parse_offline_submode(value, &submode)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_offline_submode");
        }
        (void)config_manager_set_offline_submode(submode);
        cfg.offline_submode = submode;
        has_any = true;
        submode_changed = true;
    }

    if (rest_api_query_value(req, "aura_gap_ms", value, sizeof(value)) == ESP_OK) {
        uint32_t gap_ms = 0;
        if (!rest_api_parse_u32(value, &gap_ms) || gap_ms > 60000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_aura_gap_ms");
        }
        (void)config_manager_set_aura_gap_ms(gap_ms);
        cfg.aura_gap_ms = gap_ms;
        has_any = true;
        aura_changed = true;
    }

    char intro_dir[ORB_CONFIG_PATH_MAX] = { 0 };
    char response_dir[ORB_CONFIG_PATH_MAX] = { 0 };
    bool intro_changed = false;
    bool response_changed = false;
    if (rest_api_query_value(req, "aura_intro_dir", intro_dir, sizeof(intro_dir)) == ESP_OK) {
        intro_changed = true;
    }
    if (rest_api_query_value(req, "aura_response_dir", response_dir, sizeof(response_dir)) == ESP_OK) {
        response_changed = true;
    }
    if (intro_changed || response_changed) {
        const char *intro = intro_changed ? intro_dir : cfg.aura_intro_dir;
        const char *response = response_changed ? response_dir : cfg.aura_response_dir;
        if (config_manager_set_aura_directories(intro, response) != ESP_OK) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_aura_dirs");
        }
        (void)snprintf(cfg.aura_intro_dir, sizeof(cfg.aura_intro_dir), "%s", intro);
        (void)snprintf(cfg.aura_response_dir, sizeof(cfg.aura_response_dir), "%s", response);
        has_any = true;
        aura_changed = true;
    }

    uint32_t reject_th_u32 = cfg.hybrid_reject_threshold_permille;
    uint32_t mic_capture_ms = cfg.hybrid_mic_capture_ms;
    uint32_t unknown_retry_u32 = cfg.hybrid_unknown_retry_max;
    if (rest_api_query_value(req, "hybrid_reject_threshold_permille", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &reject_th_u32) || reject_th_u32 > 1000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_reject_threshold");
        }
        hybrid_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_mic_capture_ms", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &mic_capture_ms) || mic_capture_ms < 1000U || mic_capture_ms > 60000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_mic_capture_ms");
        }
        hybrid_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_unknown_retry_max", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &unknown_retry_u32) || unknown_retry_u32 > 2U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_unknown_retry_max");
        }
        hybrid_retry_changed = true;
    }
    if (hybrid_changed) {
        if (config_manager_set_hybrid_params((uint16_t)reject_th_u32, mic_capture_ms) != ESP_OK) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_params");
        }
        cfg.hybrid_reject_threshold_permille = (uint16_t)reject_th_u32;
        cfg.hybrid_mic_capture_ms = mic_capture_ms;
        has_any = true;
    }
    if (hybrid_retry_changed) {
        if (config_manager_set_hybrid_unknown_retry_max((uint8_t)unknown_retry_u32) != ESP_OK) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_unknown_retry_max");
        }
        cfg.hybrid_unknown_retry_max = (uint8_t)unknown_retry_u32;
        has_any = true;
    }

    uint32_t bg_fade_out_ms = cfg.prophecy_bg_fade_out_ms;
    if (rest_api_query_value(req, "prophecy_bg_fade_out_ms", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &bg_fade_out_ms) || bg_fade_out_ms > 60000U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_bg_fade_out_ms");
        }
        bg_fade_out_changed = true;
    }
    if (bg_fade_out_changed) {
        if (config_manager_set_prophecy_background(cfg.prophecy_bg_gain_permille,
                                                   cfg.prophecy_bg_fade_in_ms,
                                                   bg_fade_out_ms) != ESP_OK) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_prophecy_bg_fade_out_ms");
        }
        cfg.prophecy_bg_fade_out_ms = bg_fade_out_ms;
        has_any = true;
    }

    bool network = cfg.network_enabled;
    bool mqtt = cfg.mqtt_enabled;
    bool ai = cfg.ai_enabled;
    bool web = cfg.web_enabled;
    bool parsed = false;

    if (rest_api_query_value(req, "network", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_bool_text(value, &network)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_network");
        }
        parsed = true;
    }
    if (rest_api_query_value(req, "mqtt", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_bool_text(value, &mqtt)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_mqtt");
        }
        parsed = true;
    }
    if (rest_api_query_value(req, "ai", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_bool_text(value, &ai)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_ai");
        }
        parsed = true;
    }
    if (rest_api_query_value(req, "web", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_bool_text(value, &web)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_web");
        }
        parsed = true;
    }
    if (parsed) {
        (void)config_manager_set_feature_flags(network, mqtt, ai, web);
        cfg.network_enabled = network;
        cfg.mqtt_enabled = mqtt;
        cfg.ai_enabled = ai;
        cfg.web_enabled = web;
        has_any = true;
        flags_changed = true;
    }

    bool persist = false;
    if (rest_api_query_value(req, "save", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_bool_text(value, &persist)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_save");
        }
    }
    if (persist) {
        (void)config_manager_save();
        ESP_LOGI(TAG, "config save requested uri=%s", req ? req->uri : "-");
    }

    if (!has_any) {
        return rest_api_send_error_json(req, "400 Bad Request", "no_valid_params");
    }

    ESP_LOGI(
        TAG,
        "config apply uri=%s changed=[brightness:%d volume:%d submode:%d aura:%d hybrid:%d hybrid_retry:%d bg_fade_out:%d flags:%d] "
        "now[brightness=%u volume=%u submode=%s reject=%u mic_ms=%" PRIu32 " retries=%u bg_fade_out=%" PRIu32 "] save=%d",
        req ? req->uri : "-",
        (int)brightness_changed,
        (int)volume_changed,
        (int)submode_changed,
        (int)aura_changed,
        (int)hybrid_changed,
        (int)hybrid_retry_changed,
        (int)bg_fade_out_changed,
        (int)flags_changed,
        cfg.led_brightness,
        cfg.audio_volume,
        config_manager_offline_submode_to_str(cfg.offline_submode),
        (unsigned)cfg.hybrid_reject_threshold_permille,
        cfg.hybrid_mic_capture_ms,
        (unsigned)cfg.hybrid_unknown_retry_max,
        cfg.prophecy_bg_fade_out_ms,
        (int)persist
    );

    return send_config_snapshot(req);
}

esp_err_t rest_api_register_config_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t get_cfg = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t post_cfg = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_cfg), TAG, "register config GET failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &post_cfg), TAG, "register config POST failed");
    return ESP_OK;
}
