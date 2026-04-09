#ifndef APP_API_H
#define APP_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_defs.h"
#include "config_schema.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_API_MODE_NAME_MAX 32U
#define APP_API_FSM_NAME_MAX 24U
#define APP_API_SESSION_STATE_NAME_MAX 24U
#define APP_API_PROFILE_NAME_MAX 16U
#define APP_API_LINK_STATE_NAME_MAX 16U
#define APP_API_IP_ADDR_MAX 16U
#define APP_API_AURA_COLOR_MAX 24U
#define APP_API_OFFLINE_SUBMODE_NAME_MAX 24U
#define APP_API_STA_SSID_MAX 33U
#define APP_API_STA_PASSWORD_MAX 65U

typedef struct {
    bool network_up;
    bool sta_password_set;
    char desired_profile[APP_API_PROFILE_NAME_MAX];
    char active_profile[APP_API_PROFILE_NAME_MAX];
    char link_state[APP_API_LINK_STATE_NAME_MAX];
    char sta_ssid[APP_API_STA_SSID_MAX];
    char sta_ip[APP_API_IP_ADDR_MAX];
    char ap_ip[APP_API_IP_ADDR_MAX];
} app_api_network_status_t;

typedef struct {
    orb_mode_t current_mode;
    char mode_name[APP_API_MODE_NAME_MAX];
    char fsm_name[APP_API_FSM_NAME_MAX];
    uint32_t session_id;
    bool session_active;
    char session_state_name[APP_API_SESSION_STATE_NAME_MAX];
    uint8_t brightness;
    uint8_t volume;
    bool network_enabled;
    bool mqtt_enabled;
    bool ai_enabled;
    bool web_enabled;
    char offline_submode_name[APP_API_OFFLINE_SUBMODE_NAME_MAX];
    uint32_t aura_gap_ms;
    char aura_selected_color[APP_API_AURA_COLOR_MAX];
    uint16_t hybrid_reject_threshold_permille;
    uint32_t hybrid_mic_capture_ms;
    bool network_up;
    char link_state_name[APP_API_LINK_STATE_NAME_MAX];
    char desired_profile_name[APP_API_PROFILE_NAME_MAX];
    char active_profile_name[APP_API_PROFILE_NAME_MAX];
    char sta_ip[APP_API_IP_ADDR_MAX];
    char ap_ip[APP_API_IP_ADDR_MAX];
} app_api_status_snapshot_t;

typedef struct {
    uint32_t session_id;
    bool active;
    char state_name[APP_API_SESSION_STATE_NAME_MAX];
} app_api_session_info_t;

orb_mode_t app_api_get_current_mode(void);
const char *app_api_mode_to_str(orb_mode_t mode);
const char *app_api_mode_home_uri(orb_mode_t mode);
esp_err_t app_api_request_mode_switch(orb_mode_t target_mode);

esp_err_t app_api_get_network_status(app_api_network_status_t *out_status);
esp_err_t app_api_apply_sta_credentials_and_reconfigure(const char *ssid, const char *password, bool persist);

esp_err_t app_api_get_status_snapshot(app_api_status_snapshot_t *out_snapshot);
esp_err_t app_api_get_session_info(app_api_session_info_t *out_info);

esp_err_t app_api_get_offline_submode(orb_offline_submode_t *out_submode);
esp_err_t app_api_set_offline_submode(orb_offline_submode_t submode);
const char *app_api_offline_submode_to_str(orb_offline_submode_t submode);
bool app_api_parse_offline_submode(const char *text, orb_offline_submode_t *out_submode);

esp_err_t app_api_get_runtime_config(orb_runtime_config_t *out_cfg);
esp_err_t app_api_save_runtime_config(void);
esp_err_t app_api_set_led_brightness(uint8_t brightness, bool apply_now);
esp_err_t app_api_set_audio_volume(uint8_t volume, bool apply_now);
esp_err_t app_api_set_feature_flags(bool network_enabled, bool mqtt_enabled, bool ai_enabled, bool web_enabled);
esp_err_t app_api_set_aura_gap_ms(uint32_t gap_ms);
esp_err_t app_api_set_aura_directories(const char *intro_dir, const char *response_dir);
esp_err_t app_api_set_prophecy_timing(uint32_t gap12_ms, uint32_t gap23_ms, uint32_t gap34_ms, uint32_t leadin_wait_ms);
esp_err_t app_api_set_hybrid_params(uint16_t reject_threshold_permille, uint32_t mic_capture_ms);
esp_err_t app_api_set_hybrid_unknown_retry_max(uint8_t unknown_retry_max);
esp_err_t app_api_set_prophecy_background(uint16_t gain_permille, uint32_t fade_in_ms, uint32_t fade_out_ms);
esp_err_t app_api_set_hybrid_effect(uint32_t idle_scene_id,
                                    uint32_t talk_scene_id,
                                    uint8_t speed,
                                    uint8_t intensity,
                                    uint8_t scale,
                                    bool apply_now);
esp_err_t app_api_set_hybrid_effect_palette(uint8_t palette_mode,
                                            uint8_t color1_r,
                                            uint8_t color1_g,
                                            uint8_t color1_b,
                                            uint8_t color2_r,
                                            uint8_t color2_g,
                                            uint8_t color2_b,
                                            uint8_t color3_r,
                                            uint8_t color3_g,
                                            uint8_t color3_b,
                                            bool apply_now);

esp_err_t app_api_request_offline_lottery_start(void);
esp_err_t app_api_set_offline_lottery_settings(uint8_t team_count,
                                               uint16_t participants_total,
                                               const orb_lottery_team_config_t *teams,
                                               size_t teams_count,
                                               uint8_t finish_source,
                                               const char *finish_value);
esp_err_t app_api_audio_stop(void);

#ifdef __cplusplus
}
#endif

#endif
