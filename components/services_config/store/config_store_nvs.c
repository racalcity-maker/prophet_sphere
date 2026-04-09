#include "config_store_nvs.h"

#include <stddef.h>
#include <stdint.h>
#include "nvs.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_CONFIG_PERSIST_TO_NVS
#define CONFIG_ORB_CONFIG_PERSIST_TO_NVS 1
#endif

#ifndef CONFIG_ORB_CONFIG_NVS_NAMESPACE
#define CONFIG_ORB_CONFIG_NVS_NAMESPACE "orb_cfg"
#endif

#define ORB_CFG_NVS_VERSION 9U

static const char *KEY_VER = "ver";
static const char *KEY_LED_BRIGHTNESS = "led_bri";
static const char *KEY_AUDIO_VOLUME = "aud_vol";
static const char *KEY_FEATURE_FLAGS = "flags";
static const char *KEY_OFFLINE_SUBMODE = "off_sub";
static const char *KEY_AURA_GAP_MS = "a_gap";
static const char *KEY_AURA_INTRO_DIR = "a_intro";
static const char *KEY_AURA_RESPONSE_DIR = "a_resp";
static const char *KEY_PROPHECY_GAP12_MS = "p_g12";
static const char *KEY_PROPHECY_GAP23_MS = "p_g23";
static const char *KEY_PROPHECY_GAP34_MS = "p_g34";
static const char *KEY_PROPHECY_LEADIN_MS = "p_lead";
static const char *KEY_HYBRID_REJECT_TH = "h_rjct";
static const char *KEY_HYBRID_MIC_CAPTURE_MS = "h_mcap";
static const char *KEY_HYBRID_UNKNOWN_RETRY = "h_unkr";
static const char *KEY_HYBRID_EFFECT_IDLE_SCENE = "h_eiscn";
static const char *KEY_HYBRID_EFFECT_TALK_SCENE = "h_etscn";
static const char *KEY_HYBRID_EFFECT_SCENE = "h_escn";
static const char *KEY_HYBRID_EFFECT_SPEED = "h_espd";
static const char *KEY_HYBRID_EFFECT_INTENSITY = "h_eint";
static const char *KEY_HYBRID_EFFECT_SCALE = "h_escl";
static const char *KEY_HYBRID_EFFECT_PAL_MODE = "h_epm";
static const char *KEY_HYBRID_EFFECT_C1_R = "h_ec1r";
static const char *KEY_HYBRID_EFFECT_C1_G = "h_ec1g";
static const char *KEY_HYBRID_EFFECT_C1_B = "h_ec1b";
static const char *KEY_HYBRID_EFFECT_C2_R = "h_ec2r";
static const char *KEY_HYBRID_EFFECT_C2_G = "h_ec2g";
static const char *KEY_HYBRID_EFFECT_C2_B = "h_ec2b";
static const char *KEY_HYBRID_EFFECT_C3_R = "h_ec3r";
static const char *KEY_HYBRID_EFFECT_C3_G = "h_ec3g";
static const char *KEY_HYBRID_EFFECT_C3_B = "h_ec3b";
static const char *KEY_PROPHECY_BG_GAIN = "p_bgg";
static const char *KEY_PROPHECY_BG_FADE_IN_MS = "p_bfi";
static const char *KEY_PROPHECY_BG_FADE_OUT_MS = "p_bfo";
static const char *KEY_WIFI_STA_SSID = "w_ssid";
static const char *KEY_WIFI_STA_PASSWORD = "w_pass";
static const char *KEY_LOTTERY_TEAM_COUNT = "l_tcnt";
static const char *KEY_LOTTERY_PARTICIPANTS = "l_part";
static const char *KEY_LOTTERY_TEAM_SRC[ORB_LOTTERY_MAX_TEAMS] = { "l1_src", "l2_src", "l3_src", "l4_src" };
static const char *KEY_LOTTERY_TEAM_R[ORB_LOTTERY_MAX_TEAMS] = { "l1_r", "l2_r", "l3_r", "l4_r" };
static const char *KEY_LOTTERY_TEAM_G[ORB_LOTTERY_MAX_TEAMS] = { "l1_g", "l2_g", "l3_g", "l4_g" };
static const char *KEY_LOTTERY_TEAM_B[ORB_LOTTERY_MAX_TEAMS] = { "l1_b", "l2_b", "l3_b", "l4_b" };
static const char *KEY_LOTTERY_TEAM_TRACK[ORB_LOTTERY_MAX_TEAMS] = { "l1_trk", "l2_trk", "l3_trk", "l4_trk" };
static const char *KEY_LOTTERY_TEAM_TTS[ORB_LOTTERY_MAX_TEAMS] = { "l1_tts", "l2_tts", "l3_tts", "l4_tts" };
static const char *KEY_LOTTERY_FINISH_SRC = "l_fsrc";
static const char *KEY_LOTTERY_FINISH_VAL = "l_fval";

enum {
    FEATURE_FLAG_NETWORK = (1U << 0),
    FEATURE_FLAG_MQTT = (1U << 1),
    FEATURE_FLAG_AI = (1U << 2),
    FEATURE_FLAG_WEB = (1U << 3),
};

static uint8_t pack_feature_flags(const orb_runtime_config_t *cfg)
{
    uint8_t flags = 0;
    if (cfg->network_enabled) {
        flags |= FEATURE_FLAG_NETWORK;
    }
    if (cfg->mqtt_enabled) {
        flags |= FEATURE_FLAG_MQTT;
    }
    if (cfg->ai_enabled) {
        flags |= FEATURE_FLAG_AI;
    }
    if (cfg->web_enabled) {
        flags |= FEATURE_FLAG_WEB;
    }
    return flags;
}

static void unpack_feature_flags(uint8_t flags, orb_runtime_config_t *cfg)
{
    cfg->network_enabled = ((flags & FEATURE_FLAG_NETWORK) != 0U);
    cfg->mqtt_enabled = ((flags & FEATURE_FLAG_MQTT) != 0U);
    cfg->ai_enabled = ((flags & FEATURE_FLAG_AI) != 0U);
    cfg->web_enabled = ((flags & FEATURE_FLAG_WEB) != 0U);
}

static bool offline_submode_valid(uint8_t v)
{
    return (v <= (uint8_t)ORB_OFFLINE_SUBMODE_PROPHECY);
}

static bool lottery_team_source_valid(uint8_t v)
{
    return v <= (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TTS;
}

esp_err_t config_store_nvs_load(orb_runtime_config_t *cfg, bool *loaded)
{
    if (cfg == NULL || loaded == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *loaded = false;

#if !CONFIG_ORB_CONFIG_PERSIST_TO_NVS
    return ESP_OK;
#else
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_ORB_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t version = 0;
    err = nvs_get_u8(handle, KEY_VER, &version);
    if (err != ESP_OK) {
        nvs_close(handle);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    if (version != ORB_CFG_NVS_VERSION) {
        nvs_close(handle);
        return ESP_ERR_INVALID_VERSION;
    }

    uint8_t value_u8 = 0;
    if (nvs_get_u8(handle, KEY_LED_BRIGHTNESS, &value_u8) == ESP_OK) {
        cfg->led_brightness = value_u8;
    }
    if (nvs_get_u8(handle, KEY_AUDIO_VOLUME, &value_u8) == ESP_OK) {
        cfg->audio_volume = value_u8;
    }
    if (nvs_get_u8(handle, KEY_FEATURE_FLAGS, &value_u8) == ESP_OK) {
        unpack_feature_flags(value_u8, cfg);
    }
    if (nvs_get_u8(handle, KEY_OFFLINE_SUBMODE, &value_u8) == ESP_OK && offline_submode_valid(value_u8)) {
        cfg->offline_submode = (orb_offline_submode_t)value_u8;
    }
    if (nvs_get_u8(handle, KEY_LOTTERY_TEAM_COUNT, &value_u8) == ESP_OK) {
        if (value_u8 >= 2U && value_u8 <= ORB_LOTTERY_MAX_TEAMS) {
            cfg->offline_lottery_team_count = value_u8;
        }
    }
    uint16_t value_u16 = 0U;
    if (nvs_get_u16(handle, KEY_LOTTERY_PARTICIPANTS, &value_u16) == ESP_OK) {
        if (value_u16 >= 1U && value_u16 <= ORB_LOTTERY_PARTICIPANTS_MAX) {
            cfg->offline_lottery_participants_total = value_u16;
        }
    }
    for (size_t i = 0U; i < ORB_LOTTERY_MAX_TEAMS; ++i) {
        if (nvs_get_u8(handle, KEY_LOTTERY_TEAM_SRC[i], &value_u8) == ESP_OK && lottery_team_source_valid(value_u8)) {
            cfg->offline_lottery_teams[i].source = value_u8;
        }
        if (nvs_get_u8(handle, KEY_LOTTERY_TEAM_R[i], &value_u8) == ESP_OK) {
            cfg->offline_lottery_teams[i].color_r = value_u8;
        }
        if (nvs_get_u8(handle, KEY_LOTTERY_TEAM_G[i], &value_u8) == ESP_OK) {
            cfg->offline_lottery_teams[i].color_g = value_u8;
        }
        if (nvs_get_u8(handle, KEY_LOTTERY_TEAM_B[i], &value_u8) == ESP_OK) {
            cfg->offline_lottery_teams[i].color_b = value_u8;
        }
        size_t str_len = sizeof(cfg->offline_lottery_teams[i].track_path);
        (void)nvs_get_str(handle, KEY_LOTTERY_TEAM_TRACK[i], cfg->offline_lottery_teams[i].track_path, &str_len);
        str_len = sizeof(cfg->offline_lottery_teams[i].tts_text);
        (void)nvs_get_str(handle, KEY_LOTTERY_TEAM_TTS[i], cfg->offline_lottery_teams[i].tts_text, &str_len);
    }
    if (nvs_get_u8(handle, KEY_LOTTERY_FINISH_SRC, &value_u8) == ESP_OK && lottery_team_source_valid(value_u8)) {
        cfg->offline_lottery_finish_source = value_u8;
    }
    size_t finish_len = sizeof(cfg->offline_lottery_finish_value);
    (void)nvs_get_str(handle, KEY_LOTTERY_FINISH_VAL, cfg->offline_lottery_finish_value, &finish_len);

    uint32_t gap_ms = 0;
    if (nvs_get_u32(handle, KEY_AURA_GAP_MS, &gap_ms) == ESP_OK) {
        cfg->aura_gap_ms = gap_ms;
    }
    if (nvs_get_u32(handle, KEY_PROPHECY_GAP12_MS, &gap_ms) == ESP_OK) {
        cfg->prophecy_gap12_ms = gap_ms;
    }
    if (nvs_get_u32(handle, KEY_PROPHECY_GAP23_MS, &gap_ms) == ESP_OK) {
        cfg->prophecy_gap23_ms = gap_ms;
    }
    if (nvs_get_u32(handle, KEY_PROPHECY_GAP34_MS, &gap_ms) == ESP_OK) {
        cfg->prophecy_gap34_ms = gap_ms;
    }
    if (nvs_get_u32(handle, KEY_PROPHECY_LEADIN_MS, &gap_ms) == ESP_OK) {
        cfg->prophecy_leadin_wait_ms = gap_ms;
    }
    uint16_t reject_th = 0U;
    if (nvs_get_u16(handle, KEY_HYBRID_REJECT_TH, &reject_th) == ESP_OK) {
        cfg->hybrid_reject_threshold_permille = reject_th;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_UNKNOWN_RETRY, &value_u8) == ESP_OK) {
        if (value_u8 <= 2U) {
            cfg->hybrid_unknown_retry_max = value_u8;
        }
    }
    if (nvs_get_u32(handle, KEY_HYBRID_EFFECT_IDLE_SCENE, &gap_ms) == ESP_OK) {
        cfg->hybrid_effect_idle_scene_id = gap_ms;
    }
    if (nvs_get_u32(handle, KEY_HYBRID_EFFECT_TALK_SCENE, &gap_ms) == ESP_OK) {
        cfg->hybrid_effect_talk_scene_id = gap_ms;
    } else if (nvs_get_u32(handle, KEY_HYBRID_EFFECT_SCENE, &gap_ms) == ESP_OK) {
        /* Backward compatibility with legacy single-scene key. */
        cfg->hybrid_effect_talk_scene_id = gap_ms;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_SPEED, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_speed = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_INTENSITY, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_intensity = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_SCALE, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_scale = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_PAL_MODE, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_palette_mode = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_C1_R, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_color1_r = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_C1_G, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_color1_g = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_C1_B, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_color1_b = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_C2_R, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_color2_r = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_C2_G, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_color2_g = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_C2_B, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_color2_b = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_C3_R, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_color3_r = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_C3_G, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_color3_g = value_u8;
    }
    if (nvs_get_u8(handle, KEY_HYBRID_EFFECT_C3_B, &value_u8) == ESP_OK) {
        cfg->hybrid_effect_color3_b = value_u8;
    }
    if (nvs_get_u32(handle, KEY_HYBRID_MIC_CAPTURE_MS, &gap_ms) == ESP_OK) {
        cfg->hybrid_mic_capture_ms = gap_ms;
    }
    if (nvs_get_u32(handle, KEY_PROPHECY_BG_FADE_IN_MS, &gap_ms) == ESP_OK) {
        cfg->prophecy_bg_fade_in_ms = gap_ms;
    }
    if (nvs_get_u32(handle, KEY_PROPHECY_BG_FADE_OUT_MS, &gap_ms) == ESP_OK) {
        cfg->prophecy_bg_fade_out_ms = gap_ms;
    }
    uint16_t bg_gain = 0U;
    if (nvs_get_u16(handle, KEY_PROPHECY_BG_GAIN, &bg_gain) == ESP_OK) {
        cfg->prophecy_bg_gain_permille = bg_gain;
    }

    size_t str_len = sizeof(cfg->aura_intro_dir);
    (void)nvs_get_str(handle, KEY_AURA_INTRO_DIR, cfg->aura_intro_dir, &str_len);
    str_len = sizeof(cfg->aura_response_dir);
    (void)nvs_get_str(handle, KEY_AURA_RESPONSE_DIR, cfg->aura_response_dir, &str_len);
    str_len = sizeof(cfg->wifi_sta_ssid);
    (void)nvs_get_str(handle, KEY_WIFI_STA_SSID, cfg->wifi_sta_ssid, &str_len);
    str_len = sizeof(cfg->wifi_sta_password);
    (void)nvs_get_str(handle, KEY_WIFI_STA_PASSWORD, cfg->wifi_sta_password, &str_len);

    nvs_close(handle);
    *loaded = true;
    return ESP_OK;
#endif
}

esp_err_t config_store_nvs_save(const orb_runtime_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_ORB_CONFIG_PERSIST_TO_NVS
    return ESP_OK;
#else
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_ORB_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, KEY_VER, ORB_CFG_NVS_VERSION);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_LED_BRIGHTNESS, cfg->led_brightness);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_AUDIO_VOLUME, cfg->audio_volume);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_FEATURE_FLAGS, pack_feature_flags(cfg));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_OFFLINE_SUBMODE, (uint8_t)cfg->offline_submode);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_LOTTERY_TEAM_COUNT, cfg->offline_lottery_team_count);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, KEY_LOTTERY_PARTICIPANTS, cfg->offline_lottery_participants_total);
    }
    for (size_t i = 0U; err == ESP_OK && i < ORB_LOTTERY_MAX_TEAMS; ++i) {
        err = nvs_set_u8(handle, KEY_LOTTERY_TEAM_SRC[i], cfg->offline_lottery_teams[i].source);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, KEY_LOTTERY_TEAM_R[i], cfg->offline_lottery_teams[i].color_r);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, KEY_LOTTERY_TEAM_G[i], cfg->offline_lottery_teams[i].color_g);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, KEY_LOTTERY_TEAM_B[i], cfg->offline_lottery_teams[i].color_b);
        }
        if (err == ESP_OK) {
            err = nvs_set_str(handle, KEY_LOTTERY_TEAM_TRACK[i], cfg->offline_lottery_teams[i].track_path);
        }
        if (err == ESP_OK) {
            err = nvs_set_str(handle, KEY_LOTTERY_TEAM_TTS[i], cfg->offline_lottery_teams[i].tts_text);
        }
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_LOTTERY_FINISH_SRC, cfg->offline_lottery_finish_source);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_LOTTERY_FINISH_VAL, cfg->offline_lottery_finish_value);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_AURA_GAP_MS, cfg->aura_gap_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_PROPHECY_GAP12_MS, cfg->prophecy_gap12_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_PROPHECY_GAP23_MS, cfg->prophecy_gap23_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_PROPHECY_GAP34_MS, cfg->prophecy_gap34_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_PROPHECY_LEADIN_MS, cfg->prophecy_leadin_wait_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, KEY_HYBRID_REJECT_TH, cfg->hybrid_reject_threshold_permille);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_UNKNOWN_RETRY, cfg->hybrid_unknown_retry_max);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_HYBRID_EFFECT_IDLE_SCENE, cfg->hybrid_effect_idle_scene_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_HYBRID_EFFECT_TALK_SCENE, cfg->hybrid_effect_talk_scene_id);
    }
    if (err == ESP_OK) {
        /* Keep legacy key for downgrade compatibility. */
        err = nvs_set_u32(handle, KEY_HYBRID_EFFECT_SCENE, cfg->hybrid_effect_talk_scene_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_SPEED, cfg->hybrid_effect_speed);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_INTENSITY, cfg->hybrid_effect_intensity);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_SCALE, cfg->hybrid_effect_scale);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_PAL_MODE, cfg->hybrid_effect_palette_mode);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_C1_R, cfg->hybrid_effect_color1_r);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_C1_G, cfg->hybrid_effect_color1_g);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_C1_B, cfg->hybrid_effect_color1_b);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_C2_R, cfg->hybrid_effect_color2_r);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_C2_G, cfg->hybrid_effect_color2_g);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_C2_B, cfg->hybrid_effect_color2_b);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_C3_R, cfg->hybrid_effect_color3_r);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_C3_G, cfg->hybrid_effect_color3_g);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_HYBRID_EFFECT_C3_B, cfg->hybrid_effect_color3_b);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_HYBRID_MIC_CAPTURE_MS, cfg->hybrid_mic_capture_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, KEY_PROPHECY_BG_GAIN, cfg->prophecy_bg_gain_permille);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_PROPHECY_BG_FADE_IN_MS, cfg->prophecy_bg_fade_in_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_PROPHECY_BG_FADE_OUT_MS, cfg->prophecy_bg_fade_out_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_AURA_INTRO_DIR, cfg->aura_intro_dir);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_AURA_RESPONSE_DIR, cfg->aura_response_dir);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_WIFI_STA_SSID, cfg->wifi_sta_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_WIFI_STA_PASSWORD, cfg->wifi_sta_password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
#endif
}

esp_err_t config_store_nvs_has_wifi_sta_credentials(bool *has_credentials)
{
    if (has_credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *has_credentials = false;

#if !CONFIG_ORB_CONFIG_PERSIST_TO_NVS
    return ESP_OK;
#else
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_ORB_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t version = 0;
    err = nvs_get_u8(handle, KEY_VER, &version);
    if (err != ESP_OK || version != ORB_CFG_NVS_VERSION) {
        nvs_close(handle);
        return ESP_OK;
    }

    char ssid[ORB_WIFI_SSID_MAX] = { 0 };
    size_t ssid_len = sizeof(ssid);
    err = nvs_get_str(handle, KEY_WIFI_STA_SSID, ssid, &ssid_len);
    if (err == ESP_OK && ssid[0] != '\0') {
        /* Password may be empty for open networks, but key should exist. */
        size_t pass_len = 0;
        esp_err_t pass_err = nvs_get_str(handle, KEY_WIFI_STA_PASSWORD, NULL, &pass_len);
        if (pass_err == ESP_OK) {
            *has_credentials = true;
        } else if (pass_err != ESP_ERR_NVS_NOT_FOUND) {
            err = pass_err;
        }
    }

    nvs_close(handle);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
#endif
}
