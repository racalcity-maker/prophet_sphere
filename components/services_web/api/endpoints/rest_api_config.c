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
    static orb_runtime_config_t cfg;
    static char json[2048];

    esp_err_t err = config_manager_get_snapshot(&cfg);
    if (err != ESP_OK) {
        return rest_api_send_error_json(req, "500 Internal Server Error", "config_read_failed");
    }

    (void)snprintf(json,
                   sizeof(json),
                   "{\"ok\":true,\"brightness\":%u,\"volume\":%u,\"network\":%s,\"mqtt\":%s,\"ai\":%s,\"web\":%s,"
                   "\"offline_submode\":\"%s\",\"aura_gap_ms\":%" PRIu32 ","
                   "\"aura_intro_dir\":\"%s\",\"aura_response_dir\":\"%s\","
                   "\"hybrid_reject_threshold_permille\":%u,\"hybrid_mic_capture_ms\":%" PRIu32 ","
                   "\"hybrid_unknown_retry_max\":%u,\"prophecy_bg_fade_out_ms\":%" PRIu32 ","
                   "\"hybrid_effect_idle_scene_id\":%" PRIu32 ",\"hybrid_effect_talk_scene_id\":%" PRIu32 ","
                   "\"hybrid_effect_speed\":%u,"
                   "\"hybrid_effect_intensity\":%u,\"hybrid_effect_scale\":%u,"
                   "\"hybrid_effect_palette_mode\":%u,"
                   "\"hybrid_effect_color1_r\":%u,\"hybrid_effect_color1_g\":%u,\"hybrid_effect_color1_b\":%u,"
                   "\"hybrid_effect_color2_r\":%u,\"hybrid_effect_color2_g\":%u,\"hybrid_effect_color2_b\":%u,"
                   "\"hybrid_effect_color3_r\":%u,\"hybrid_effect_color3_g\":%u,\"hybrid_effect_color3_b\":%u}",
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
                   cfg.prophecy_bg_fade_out_ms,
                   cfg.hybrid_effect_idle_scene_id,
                   cfg.hybrid_effect_talk_scene_id,
                   (unsigned)cfg.hybrid_effect_speed,
                   (unsigned)cfg.hybrid_effect_intensity,
                   (unsigned)cfg.hybrid_effect_scale,
                   (unsigned)cfg.hybrid_effect_palette_mode,
                   (unsigned)cfg.hybrid_effect_color1_r,
                   (unsigned)cfg.hybrid_effect_color1_g,
                   (unsigned)cfg.hybrid_effect_color1_b,
                   (unsigned)cfg.hybrid_effect_color2_r,
                   (unsigned)cfg.hybrid_effect_color2_g,
                   (unsigned)cfg.hybrid_effect_color2_b,
                   (unsigned)cfg.hybrid_effect_color3_r,
                   (unsigned)cfg.hybrid_effect_color3_g,
                   (unsigned)cfg.hybrid_effect_color3_b);
    return rest_api_send_json(req, "200 OK", json);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "config GET uri=%s", req ? req->uri : "-");
    return send_config_snapshot(req);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    static orb_runtime_config_t cfg;
    static char intro_dir[ORB_CONFIG_PATH_MAX];
    static char response_dir[ORB_CONFIG_PATH_MAX];

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
    bool hybrid_effect_changed = false;
    bool hybrid_effect_palette_changed = false;
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

    intro_dir[0] = '\0';
    response_dir[0] = '\0';
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

    uint32_t hybrid_effect_idle_scene_id = cfg.hybrid_effect_idle_scene_id;
    uint32_t hybrid_effect_talk_scene_id = cfg.hybrid_effect_talk_scene_id;
    uint32_t hybrid_effect_speed_u32 = cfg.hybrid_effect_speed;
    uint32_t hybrid_effect_intensity_u32 = cfg.hybrid_effect_intensity;
    uint32_t hybrid_effect_scale_u32 = cfg.hybrid_effect_scale;
    uint32_t hybrid_effect_palette_mode_u32 = cfg.hybrid_effect_palette_mode;
    uint32_t hybrid_effect_color1_r_u32 = cfg.hybrid_effect_color1_r;
    uint32_t hybrid_effect_color1_g_u32 = cfg.hybrid_effect_color1_g;
    uint32_t hybrid_effect_color1_b_u32 = cfg.hybrid_effect_color1_b;
    uint32_t hybrid_effect_color2_r_u32 = cfg.hybrid_effect_color2_r;
    uint32_t hybrid_effect_color2_g_u32 = cfg.hybrid_effect_color2_g;
    uint32_t hybrid_effect_color2_b_u32 = cfg.hybrid_effect_color2_b;
    uint32_t hybrid_effect_color3_r_u32 = cfg.hybrid_effect_color3_r;
    uint32_t hybrid_effect_color3_g_u32 = cfg.hybrid_effect_color3_g;
    uint32_t hybrid_effect_color3_b_u32 = cfg.hybrid_effect_color3_b;

    if (rest_api_query_value(req, "hybrid_effect_idle_scene_id", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_idle_scene_id)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_idle_scene_id");
        }
        hybrid_effect_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_talk_scene_id", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_talk_scene_id)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_talk_scene_id");
        }
        hybrid_effect_changed = true;
    }
    /* Backward compatibility with legacy single-scene field. */
    if (rest_api_query_value(req, "hybrid_effect_scene_id", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_talk_scene_id)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_scene_id");
        }
        hybrid_effect_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_speed", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_speed_u32) || hybrid_effect_speed_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_speed");
        }
        hybrid_effect_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_intensity", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_intensity_u32) || hybrid_effect_intensity_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_intensity");
        }
        hybrid_effect_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_scale", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_scale_u32) || hybrid_effect_scale_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_scale");
        }
        hybrid_effect_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_palette_mode", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_palette_mode_u32) || hybrid_effect_palette_mode_u32 > 2U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_palette_mode");
        }
        hybrid_effect_palette_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_color1_r", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_color1_r_u32) || hybrid_effect_color1_r_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_color1_r");
        }
        hybrid_effect_palette_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_color1_g", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_color1_g_u32) || hybrid_effect_color1_g_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_color1_g");
        }
        hybrid_effect_palette_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_color1_b", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_color1_b_u32) || hybrid_effect_color1_b_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_color1_b");
        }
        hybrid_effect_palette_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_color2_r", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_color2_r_u32) || hybrid_effect_color2_r_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_color2_r");
        }
        hybrid_effect_palette_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_color2_g", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_color2_g_u32) || hybrid_effect_color2_g_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_color2_g");
        }
        hybrid_effect_palette_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_color2_b", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_color2_b_u32) || hybrid_effect_color2_b_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_color2_b");
        }
        hybrid_effect_palette_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_color3_r", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_color3_r_u32) || hybrid_effect_color3_r_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_color3_r");
        }
        hybrid_effect_palette_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_color3_g", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_color3_g_u32) || hybrid_effect_color3_g_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_color3_g");
        }
        hybrid_effect_palette_changed = true;
    }
    if (rest_api_query_value(req, "hybrid_effect_color3_b", value, sizeof(value)) == ESP_OK) {
        if (!rest_api_parse_u32(value, &hybrid_effect_color3_b_u32) || hybrid_effect_color3_b_u32 > 255U) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_color3_b");
        }
        hybrid_effect_palette_changed = true;
    }

    if (hybrid_effect_changed) {
        if (config_manager_set_hybrid_effect(hybrid_effect_idle_scene_id,
                                             hybrid_effect_talk_scene_id,
                                             (uint8_t)hybrid_effect_speed_u32,
                                             (uint8_t)hybrid_effect_intensity_u32,
                                             (uint8_t)hybrid_effect_scale_u32) != ESP_OK) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect");
        }
        cfg.hybrid_effect_idle_scene_id = hybrid_effect_idle_scene_id;
        cfg.hybrid_effect_talk_scene_id = hybrid_effect_talk_scene_id;
        cfg.hybrid_effect_speed = (uint8_t)hybrid_effect_speed_u32;
        cfg.hybrid_effect_intensity = (uint8_t)hybrid_effect_intensity_u32;
        cfg.hybrid_effect_scale = (uint8_t)hybrid_effect_scale_u32;
        (void)led_service_set_effect_params(cfg.hybrid_effect_speed,
                                            cfg.hybrid_effect_intensity,
                                            cfg.hybrid_effect_scale,
                                            request_timeout_ms());
        has_any = true;
    }
    if (hybrid_effect_palette_changed) {
        if (config_manager_set_hybrid_effect_palette((uint8_t)hybrid_effect_palette_mode_u32,
                                                     (uint8_t)hybrid_effect_color1_r_u32,
                                                     (uint8_t)hybrid_effect_color1_g_u32,
                                                     (uint8_t)hybrid_effect_color1_b_u32,
                                                     (uint8_t)hybrid_effect_color2_r_u32,
                                                     (uint8_t)hybrid_effect_color2_g_u32,
                                                     (uint8_t)hybrid_effect_color2_b_u32,
                                                     (uint8_t)hybrid_effect_color3_r_u32,
                                                     (uint8_t)hybrid_effect_color3_g_u32,
                                                     (uint8_t)hybrid_effect_color3_b_u32) != ESP_OK) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_hybrid_effect_palette");
        }
        cfg.hybrid_effect_palette_mode = (uint8_t)hybrid_effect_palette_mode_u32;
        cfg.hybrid_effect_color1_r = (uint8_t)hybrid_effect_color1_r_u32;
        cfg.hybrid_effect_color1_g = (uint8_t)hybrid_effect_color1_g_u32;
        cfg.hybrid_effect_color1_b = (uint8_t)hybrid_effect_color1_b_u32;
        cfg.hybrid_effect_color2_r = (uint8_t)hybrid_effect_color2_r_u32;
        cfg.hybrid_effect_color2_g = (uint8_t)hybrid_effect_color2_g_u32;
        cfg.hybrid_effect_color2_b = (uint8_t)hybrid_effect_color2_b_u32;
        cfg.hybrid_effect_color3_r = (uint8_t)hybrid_effect_color3_r_u32;
        cfg.hybrid_effect_color3_g = (uint8_t)hybrid_effect_color3_g_u32;
        cfg.hybrid_effect_color3_b = (uint8_t)hybrid_effect_color3_b_u32;
        (void)led_service_set_effect_palette(cfg.hybrid_effect_palette_mode,
                                             cfg.hybrid_effect_color1_r,
                                             cfg.hybrid_effect_color1_g,
                                             cfg.hybrid_effect_color1_b,
                                             cfg.hybrid_effect_color2_r,
                                             cfg.hybrid_effect_color2_g,
                                             cfg.hybrid_effect_color2_b,
                                             cfg.hybrid_effect_color3_r,
                                             cfg.hybrid_effect_color3_g,
                                             cfg.hybrid_effect_color3_b,
                                             request_timeout_ms());
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

    ESP_LOGI(TAG,
             "config apply uri=%s changed=[b:%d v:%d s:%d a:%d h:%d hr:%d he:%d hp:%d bg:%d f:%d] now[b=%u v=%u sub=%s "
             "rej=%u mic=%" PRIu32 " retry=%u save=%d]",
             req ? req->uri : "-",
             (int)brightness_changed,
             (int)volume_changed,
             (int)submode_changed,
             (int)aura_changed,
             (int)hybrid_changed,
             (int)hybrid_retry_changed,
             (int)hybrid_effect_changed,
             (int)hybrid_effect_palette_changed,
             (int)bg_fade_out_changed,
             (int)flags_changed,
             cfg.led_brightness,
             cfg.audio_volume,
             config_manager_offline_submode_to_str(cfg.offline_submode),
             (unsigned)cfg.hybrid_reject_threshold_permille,
             cfg.hybrid_mic_capture_ms,
             (unsigned)cfg.hybrid_unknown_retry_max,
             (int)persist);

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
