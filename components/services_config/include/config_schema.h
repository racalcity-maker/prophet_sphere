#ifndef CONFIG_SCHEMA_H
#define CONFIG_SCHEMA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_CONFIG_PATH_MAX 128U
#define ORB_WIFI_SSID_MAX 33U
#define ORB_WIFI_PASSWORD_MAX 65U
#define ORB_LOTTERY_MAX_TEAMS 4U
#define ORB_LOTTERY_PARTICIPANTS_MAX 128U

typedef enum {
    ORB_LOTTERY_TEAM_SOURCE_TRACK = 0,
    ORB_LOTTERY_TEAM_SOURCE_TTS = 1,
} orb_lottery_team_source_t;

typedef struct {
    uint8_t color_r;
    uint8_t color_g;
    uint8_t color_b;
    uint8_t source;
    char track_path[ORB_CONFIG_PATH_MAX];
    char tts_text[ORB_CONFIG_PATH_MAX];
} orb_lottery_team_config_t;

typedef enum {
    ORB_OFFLINE_SUBMODE_AURA = 0,
    ORB_OFFLINE_SUBMODE_LOTTERY = 1,
    ORB_OFFLINE_SUBMODE_PROPHECY = 2,
} orb_offline_submode_t;

typedef enum {
    ORB_LED_PALETTE_MODE_RAINBOW = 0,
    ORB_LED_PALETTE_MODE_DUO = 1,
    ORB_LED_PALETTE_MODE_TRIO = 2,
} orb_led_palette_mode_t;

typedef struct {
    uint8_t led_brightness;
    uint8_t audio_volume;
    bool network_enabled;
    bool mqtt_enabled;
    bool ai_enabled;
    bool web_enabled;
    orb_offline_submode_t offline_submode;
    uint32_t offline_lottery_start_seq;
    uint8_t offline_lottery_team_count;
    uint16_t offline_lottery_participants_total;
    orb_lottery_team_config_t offline_lottery_teams[ORB_LOTTERY_MAX_TEAMS];
    uint8_t offline_lottery_finish_source;
    char offline_lottery_finish_value[ORB_CONFIG_PATH_MAX];
    uint32_t aura_gap_ms;
    uint32_t hybrid_mic_capture_ms;
    uint32_t hybrid_effect_idle_scene_id;
    uint32_t hybrid_effect_talk_scene_id;
    uint32_t prophecy_gap12_ms;
    uint32_t prophecy_gap23_ms;
    uint32_t prophecy_gap34_ms;
    uint32_t prophecy_leadin_wait_ms;
    uint16_t hybrid_reject_threshold_permille;
    uint8_t hybrid_unknown_retry_max;
    uint8_t hybrid_effect_speed;
    uint8_t hybrid_effect_intensity;
    uint8_t hybrid_effect_scale;
    uint8_t hybrid_effect_palette_mode;
    uint8_t hybrid_effect_color1_r;
    uint8_t hybrid_effect_color1_g;
    uint8_t hybrid_effect_color1_b;
    uint8_t hybrid_effect_color2_r;
    uint8_t hybrid_effect_color2_g;
    uint8_t hybrid_effect_color2_b;
    uint8_t hybrid_effect_color3_r;
    uint8_t hybrid_effect_color3_g;
    uint8_t hybrid_effect_color3_b;
    uint16_t prophecy_bg_gain_permille;
    uint32_t prophecy_bg_fade_in_ms;
    uint32_t prophecy_bg_fade_out_ms;
    char wifi_sta_ssid[ORB_WIFI_SSID_MAX];
    char wifi_sta_password[ORB_WIFI_PASSWORD_MAX];
    char aura_intro_dir[ORB_CONFIG_PATH_MAX];
    char aura_response_dir[ORB_CONFIG_PATH_MAX];
    char aura_selected_color[24];
} orb_runtime_config_t;

#ifdef __cplusplus
}
#endif

#endif
