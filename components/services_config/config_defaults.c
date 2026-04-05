#include "config_defaults.h"

#include <stdio.h>
#include "sdkconfig.h"

#ifndef CONFIG_ORB_CFG_DEFAULT_AURA_INTRO_DIR
#define CONFIG_ORB_CFG_DEFAULT_AURA_INTRO_DIR "/sdcard/audio/aura/intro"
#endif

#ifndef CONFIG_ORB_CFG_DEFAULT_AURA_RESPONSE_DIR
#define CONFIG_ORB_CFG_DEFAULT_AURA_RESPONSE_DIR "/sdcard/audio/aura/response"
#endif

#ifndef CONFIG_ORB_CFG_DEFAULT_AURA_GAP_MS
#define CONFIG_ORB_CFG_DEFAULT_AURA_GAP_MS 1200
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE
#define CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE 260
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS
#define CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS 2000
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS
#define CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS 4000
#endif
#ifndef CONFIG_ORB_HYBRID_REJECT_THRESHOLD_PERMILLE
#define CONFIG_ORB_HYBRID_REJECT_THRESHOLD_PERMILLE 380
#endif
#ifndef CONFIG_ORB_HYBRID_MIC_CAPTURE_DEFAULT_MS
#define CONFIG_ORB_HYBRID_MIC_CAPTURE_DEFAULT_MS 5000
#endif
#ifndef CONFIG_ORB_HYBRID_UNKNOWN_RETRY_MAX
#define CONFIG_ORB_HYBRID_UNKNOWN_RETRY_MAX 1
#endif
#ifndef CONFIG_ORB_CFG_DEFAULT_OFFLINE_SUBMODE_LOTTERY
#define CONFIG_ORB_CFG_DEFAULT_OFFLINE_SUBMODE_LOTTERY 0
#endif
#ifndef CONFIG_ORB_CFG_DEFAULT_OFFLINE_SUBMODE_PROPHECY
#define CONFIG_ORB_CFG_DEFAULT_OFFLINE_SUBMODE_PROPHECY 0
#endif
#ifndef CONFIG_ORB_NETWORK_STA_SSID
#define CONFIG_ORB_NETWORK_STA_SSID ""
#endif
#ifndef CONFIG_ORB_NETWORK_STA_PASSWORD
#define CONFIG_ORB_NETWORK_STA_PASSWORD ""
#endif

#define ORB_PROPHECY_GAP12_DEFAULT_MS 800U
#define ORB_PROPHECY_GAP23_DEFAULT_MS 800U
#define ORB_PROPHECY_GAP34_DEFAULT_MS 2000U
#define ORB_PROPHECY_LEADIN_DEFAULT_MS 1000U

orb_runtime_config_t config_defaults_load(void)
{
    orb_runtime_config_t cfg = {
        .led_brightness = CONFIG_ORB_LED_DEFAULT_BRIGHTNESS,
        .audio_volume = CONFIG_ORB_AUDIO_DEFAULT_VOLUME,
        .network_enabled = CONFIG_ORB_ENABLE_NETWORK,
        .mqtt_enabled = CONFIG_ORB_ENABLE_MQTT,
        .ai_enabled = CONFIG_ORB_ENABLE_AI,
        .web_enabled = CONFIG_ORB_ENABLE_WEB,
#if CONFIG_ORB_CFG_DEFAULT_OFFLINE_SUBMODE_LOTTERY
        .offline_submode = ORB_OFFLINE_SUBMODE_LOTTERY,
#elif CONFIG_ORB_CFG_DEFAULT_OFFLINE_SUBMODE_PROPHECY
        .offline_submode = ORB_OFFLINE_SUBMODE_PROPHECY,
#else
        .offline_submode = ORB_OFFLINE_SUBMODE_AURA,
#endif
        .offline_lottery_start_seq = 0U,
        .aura_gap_ms = CONFIG_ORB_CFG_DEFAULT_AURA_GAP_MS,
        .hybrid_mic_capture_ms = (uint32_t)CONFIG_ORB_HYBRID_MIC_CAPTURE_DEFAULT_MS,
        .prophecy_gap12_ms = ORB_PROPHECY_GAP12_DEFAULT_MS,
        .prophecy_gap23_ms = ORB_PROPHECY_GAP23_DEFAULT_MS,
        .prophecy_gap34_ms = ORB_PROPHECY_GAP34_DEFAULT_MS,
        .prophecy_leadin_wait_ms = ORB_PROPHECY_LEADIN_DEFAULT_MS,
        .hybrid_reject_threshold_permille = (uint16_t)CONFIG_ORB_HYBRID_REJECT_THRESHOLD_PERMILLE,
        .hybrid_unknown_retry_max = (uint8_t)CONFIG_ORB_HYBRID_UNKNOWN_RETRY_MAX,
        .prophecy_bg_gain_permille = CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE,
        .prophecy_bg_fade_in_ms = CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS,
        .prophecy_bg_fade_out_ms = CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS,
    };

    (void)snprintf(cfg.aura_intro_dir, sizeof(cfg.aura_intro_dir), "%s", CONFIG_ORB_CFG_DEFAULT_AURA_INTRO_DIR);
    (void)snprintf(cfg.aura_response_dir, sizeof(cfg.aura_response_dir), "%s", CONFIG_ORB_CFG_DEFAULT_AURA_RESPONSE_DIR);
    (void)snprintf(cfg.wifi_sta_ssid, sizeof(cfg.wifi_sta_ssid), "%s", CONFIG_ORB_NETWORK_STA_SSID);
    (void)snprintf(cfg.wifi_sta_password, sizeof(cfg.wifi_sta_password), "%s", CONFIG_ORB_NETWORK_STA_PASSWORD);
    cfg.aura_selected_color[0] = '\0';
    return cfg;
}
