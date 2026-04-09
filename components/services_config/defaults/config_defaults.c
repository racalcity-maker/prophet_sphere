#include "config_defaults.h"

#include <stddef.h>
#include "orb_led_scenes.h"
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
#ifndef CONFIG_ORB_HYBRID_EFFECT_SCENE_ID
#define CONFIG_ORB_HYBRID_EFFECT_SCENE_ID ORB_LED_SCENE_ID_HYBRID_VORTEX
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_IDLE_SCENE_ID
#define CONFIG_ORB_HYBRID_EFFECT_IDLE_SCENE_ID ORB_LED_SCENE_ID_HYBRID_IDLE_SLOW_BREATHE
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_TALK_SCENE_ID
#define CONFIG_ORB_HYBRID_EFFECT_TALK_SCENE_ID CONFIG_ORB_HYBRID_EFFECT_SCENE_ID
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_SPEED_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_SPEED_DEFAULT 170
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_INTENSITY_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_INTENSITY_DEFAULT 180
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_SCALE_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_SCALE_DEFAULT 140
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_PALETTE_MODE_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_PALETTE_MODE_DEFAULT ORB_LED_PALETTE_MODE_RAINBOW
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR1_R_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR1_R_DEFAULT 255
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR1_G_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR1_G_DEFAULT 0
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR1_B_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR1_B_DEFAULT 180
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR2_R_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR2_R_DEFAULT 0
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR2_G_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR2_G_DEFAULT 230
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR2_B_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR2_B_DEFAULT 255
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR3_R_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR3_R_DEFAULT 255
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR3_G_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR3_G_DEFAULT 190
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR3_B_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR3_B_DEFAULT 40
#endif
#ifndef CONFIG_ORB_CFG_DEFAULT_OFFLINE_SUBMODE_LOTTERY
#define CONFIG_ORB_CFG_DEFAULT_OFFLINE_SUBMODE_LOTTERY 0
#endif
#ifndef CONFIG_ORB_CFG_DEFAULT_OFFLINE_SUBMODE_PROPHECY
#define CONFIG_ORB_CFG_DEFAULT_OFFLINE_SUBMODE_PROPHECY 0
#endif
#ifndef CONFIG_ORB_OFFLINE_LOTTERY_TEAMS_COUNT
#define CONFIG_ORB_OFFLINE_LOTTERY_TEAMS_COUNT 4
#endif
#ifndef CONFIG_ORB_OFFLINE_LOTTERY_PARTICIPANTS_TOTAL
#define CONFIG_ORB_OFFLINE_LOTTERY_PARTICIPANTS_TOTAL 17
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

static const orb_runtime_config_t s_defaults_template = {
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
    .offline_lottery_team_count = (uint8_t)CONFIG_ORB_OFFLINE_LOTTERY_TEAMS_COUNT,
    .offline_lottery_participants_total = (uint16_t)CONFIG_ORB_OFFLINE_LOTTERY_PARTICIPANTS_TOTAL,
    .offline_lottery_teams =
        {
            {
                .color_r = 255U,
                .color_g = 60U,
                .color_b = 40U,
                .source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK,
                .track_path = "",
                .tts_text = "Команда один",
            },
            {
                .color_r = 40U,
                .color_g = 255U,
                .color_b = 120U,
                .source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK,
                .track_path = "",
                .tts_text = "Команда два",
            },
            {
                .color_r = 70U,
                .color_g = 160U,
                .color_b = 255U,
                .source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK,
                .track_path = "",
                .tts_text = "Команда три",
            },
            {
                .color_r = 255U,
                .color_g = 210U,
                .color_b = 40U,
                .source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK,
                .track_path = "",
                .tts_text = "Команда четыре",
            },
        },
    .offline_lottery_finish_source = (uint8_t)ORB_LOTTERY_TEAM_SOURCE_TRACK,
    .offline_lottery_finish_value = "",
    .aura_gap_ms = CONFIG_ORB_CFG_DEFAULT_AURA_GAP_MS,
    .hybrid_mic_capture_ms = (uint32_t)CONFIG_ORB_HYBRID_MIC_CAPTURE_DEFAULT_MS,
    .hybrid_effect_idle_scene_id = (uint32_t)CONFIG_ORB_HYBRID_EFFECT_IDLE_SCENE_ID,
    .hybrid_effect_talk_scene_id = (uint32_t)CONFIG_ORB_HYBRID_EFFECT_TALK_SCENE_ID,
    .prophecy_gap12_ms = ORB_PROPHECY_GAP12_DEFAULT_MS,
    .prophecy_gap23_ms = ORB_PROPHECY_GAP23_DEFAULT_MS,
    .prophecy_gap34_ms = ORB_PROPHECY_GAP34_DEFAULT_MS,
    .prophecy_leadin_wait_ms = ORB_PROPHECY_LEADIN_DEFAULT_MS,
    .hybrid_reject_threshold_permille = (uint16_t)CONFIG_ORB_HYBRID_REJECT_THRESHOLD_PERMILLE,
    .hybrid_unknown_retry_max = (uint8_t)CONFIG_ORB_HYBRID_UNKNOWN_RETRY_MAX,
    .hybrid_effect_speed = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_SPEED_DEFAULT,
    .hybrid_effect_intensity = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_INTENSITY_DEFAULT,
    .hybrid_effect_scale = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_SCALE_DEFAULT,
    .hybrid_effect_palette_mode = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_PALETTE_MODE_DEFAULT,
    .hybrid_effect_color1_r = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR1_R_DEFAULT,
    .hybrid_effect_color1_g = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR1_G_DEFAULT,
    .hybrid_effect_color1_b = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR1_B_DEFAULT,
    .hybrid_effect_color2_r = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR2_R_DEFAULT,
    .hybrid_effect_color2_g = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR2_G_DEFAULT,
    .hybrid_effect_color2_b = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR2_B_DEFAULT,
    .hybrid_effect_color3_r = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR3_R_DEFAULT,
    .hybrid_effect_color3_g = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR3_G_DEFAULT,
    .hybrid_effect_color3_b = (uint8_t)CONFIG_ORB_HYBRID_EFFECT_PALETTE_COLOR3_B_DEFAULT,
    .prophecy_bg_gain_permille = CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE,
    .prophecy_bg_fade_in_ms = CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS,
    .prophecy_bg_fade_out_ms = CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS,
    .wifi_sta_ssid = CONFIG_ORB_NETWORK_STA_SSID,
    .wifi_sta_password = CONFIG_ORB_NETWORK_STA_PASSWORD,
    .aura_intro_dir = CONFIG_ORB_CFG_DEFAULT_AURA_INTRO_DIR,
    .aura_response_dir = CONFIG_ORB_CFG_DEFAULT_AURA_RESPONSE_DIR,
    .aura_selected_color = "",
};

void config_defaults_load(orb_runtime_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    *cfg = s_defaults_template;

    uint8_t lottery_team_count = cfg->offline_lottery_team_count;
    if (lottery_team_count < 2U) {
        lottery_team_count = 2U;
    }
    if (lottery_team_count > ORB_LOTTERY_MAX_TEAMS) {
        lottery_team_count = ORB_LOTTERY_MAX_TEAMS;
    }
    cfg->offline_lottery_team_count = lottery_team_count;

    uint16_t lottery_participants_total = cfg->offline_lottery_participants_total;
    if (lottery_participants_total == 0U) {
        lottery_participants_total = 1U;
    }
    if (lottery_participants_total > ORB_LOTTERY_PARTICIPANTS_MAX) {
        lottery_participants_total = ORB_LOTTERY_PARTICIPANTS_MAX;
    }
    cfg->offline_lottery_participants_total = lottery_participants_total;
}
