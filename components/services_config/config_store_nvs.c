#include "config_store_nvs.h"

#include <stdint.h>
#include "nvs.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_CONFIG_PERSIST_TO_NVS
#define CONFIG_ORB_CONFIG_PERSIST_TO_NVS 1
#endif

#ifndef CONFIG_ORB_CONFIG_NVS_NAMESPACE
#define CONFIG_ORB_CONFIG_NVS_NAMESPACE "orb_cfg"
#endif

#define ORB_CFG_NVS_VERSION 6U

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
static const char *KEY_PROPHECY_BG_GAIN = "p_bgg";
static const char *KEY_PROPHECY_BG_FADE_IN_MS = "p_bfi";
static const char *KEY_PROPHECY_BG_FADE_OUT_MS = "p_bfo";
static const char *KEY_WIFI_STA_SSID = "w_ssid";
static const char *KEY_WIFI_STA_PASSWORD = "w_pass";

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
