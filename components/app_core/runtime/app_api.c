#include "app_api.h"

#include <stdio.h>
#include <string.h>
#include "app_tasking.h"
#include "app_fsm.h"
#include "config_manager.h"
#include "mode_manager.h"
#include "network_manager.h"
#include "sdkconfig.h"
#include "session_controller.h"

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, dst_len, "%s", src);
}

static uint32_t app_api_queue_timeout_ms(void)
{
    return (uint32_t)CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
}

orb_mode_t app_api_get_current_mode(void)
{
    return mode_manager_get_current_mode();
}

const char *app_api_mode_to_str(orb_mode_t mode)
{
    return mode_manager_mode_to_str(mode);
}

const char *app_api_mode_home_uri(orb_mode_t mode)
{
    switch (mode) {
    case ORB_MODE_HYBRID_AI:
        return "/hybrid";
    case ORB_MODE_INSTALLATION_SLAVE:
        return "/installation";
    case ORB_MODE_OFFLINE_SCRIPTED:
    default:
        return "/offline";
    }
}

esp_err_t app_api_request_mode_switch(orb_mode_t target_mode)
{
    return mode_manager_request_switch(target_mode);
}

esp_err_t app_api_get_network_status(app_api_network_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    network_status_t net = { 0 };
    orb_runtime_config_t cfg = { 0 };
    (void)network_manager_get_status(&net);
    (void)config_manager_get_snapshot(&cfg);

    memset(out_status, 0, sizeof(*out_status));
    out_status->network_up = net.network_up;
    out_status->sta_password_set = (cfg.wifi_sta_password[0] != '\0');
    copy_text(out_status->desired_profile,
              sizeof(out_status->desired_profile),
              network_manager_profile_to_str(net.desired_profile));
    copy_text(out_status->active_profile,
              sizeof(out_status->active_profile),
              network_manager_profile_to_str(net.active_profile));
    copy_text(out_status->link_state,
              sizeof(out_status->link_state),
              network_manager_link_state_to_str(net.link_state));
    copy_text(out_status->sta_ssid, sizeof(out_status->sta_ssid), cfg.wifi_sta_ssid);
    copy_text(out_status->sta_ip, sizeof(out_status->sta_ip), net.sta_ip);
    copy_text(out_status->ap_ip, sizeof(out_status->ap_ip), net.ap_ip);
    return ESP_OK;
}

esp_err_t app_api_apply_sta_credentials_and_reconfigure(const char *ssid, const char *password, bool persist)
{
    esp_err_t err = network_manager_apply_sta_credentials(ssid, password, persist);
    if (err != ESP_OK) {
        return err;
    }
    return mode_manager_request_network_reconfigure();
}

esp_err_t app_api_get_status_snapshot(app_api_status_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    orb_runtime_config_t cfg = { 0 };
    session_info_t session = { 0 };
    network_status_t net = { 0 };
    (void)config_manager_get_snapshot(&cfg);
    (void)session_controller_get_info(&session);
    (void)network_manager_get_status(&net);

    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->current_mode = mode_manager_get_current_mode();
    copy_text(out_snapshot->mode_name, sizeof(out_snapshot->mode_name), mode_manager_mode_to_str(out_snapshot->current_mode));
    copy_text(out_snapshot->fsm_name, sizeof(out_snapshot->fsm_name), app_fsm_state_to_str(app_fsm_get_state()));
    out_snapshot->session_id = session.session_id;
    out_snapshot->session_active = session.active;
    copy_text(out_snapshot->session_state_name,
              sizeof(out_snapshot->session_state_name),
              session_controller_state_to_str(session.state));
    out_snapshot->brightness = cfg.led_brightness;
    out_snapshot->volume = cfg.audio_volume;
    out_snapshot->network_enabled = cfg.network_enabled;
    out_snapshot->mqtt_enabled = cfg.mqtt_enabled;
    out_snapshot->ai_enabled = cfg.ai_enabled;
    out_snapshot->web_enabled = cfg.web_enabled;
    copy_text(out_snapshot->offline_submode_name,
              sizeof(out_snapshot->offline_submode_name),
              config_manager_offline_submode_to_str(cfg.offline_submode));
    out_snapshot->aura_gap_ms = cfg.aura_gap_ms;
    copy_text(out_snapshot->aura_selected_color,
              sizeof(out_snapshot->aura_selected_color),
              cfg.aura_selected_color);
    out_snapshot->hybrid_reject_threshold_permille = cfg.hybrid_reject_threshold_permille;
    out_snapshot->hybrid_mic_capture_ms = cfg.hybrid_mic_capture_ms;
    out_snapshot->network_up = net.network_up;
    copy_text(out_snapshot->link_state_name,
              sizeof(out_snapshot->link_state_name),
              network_manager_link_state_to_str(net.link_state));
    copy_text(out_snapshot->desired_profile_name,
              sizeof(out_snapshot->desired_profile_name),
              network_manager_profile_to_str(net.desired_profile));
    copy_text(out_snapshot->active_profile_name,
              sizeof(out_snapshot->active_profile_name),
              network_manager_profile_to_str(net.active_profile));
    copy_text(out_snapshot->sta_ip, sizeof(out_snapshot->sta_ip), net.sta_ip);
    copy_text(out_snapshot->ap_ip, sizeof(out_snapshot->ap_ip), net.ap_ip);
    return ESP_OK;
}

esp_err_t app_api_get_session_info(app_api_session_info_t *out_info)
{
    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    session_info_t session = { 0 };
    esp_err_t err = session_controller_get_info(&session);
    if (err != ESP_OK) {
        return err;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->session_id = session.session_id;
    out_info->active = session.active;
    copy_text(out_info->state_name,
              sizeof(out_info->state_name),
              session_controller_state_to_str(session.state));
    return ESP_OK;
}

esp_err_t app_api_get_runtime_config(orb_runtime_config_t *out_cfg)
{
    return config_manager_get_snapshot(out_cfg);
}

esp_err_t app_api_save_runtime_config(void)
{
    return config_manager_save();
}

esp_err_t app_api_set_led_brightness(uint8_t brightness, bool apply_now)
{
    esp_err_t err = config_manager_set_led_brightness(brightness);
    if (err != ESP_OK || !apply_now) {
        return err;
    }
    led_command_t led_cmd = { 0 };
    led_cmd.id = LED_CMD_SET_BRIGHTNESS;
    led_cmd.payload.set_brightness.brightness = brightness;
    return app_tasking_send_led_command(&led_cmd, app_api_queue_timeout_ms());
}

esp_err_t app_api_set_audio_volume(uint8_t volume, bool apply_now)
{
    esp_err_t err = config_manager_set_audio_volume(volume);
    if (err != ESP_OK || !apply_now) {
        return err;
    }
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_SET_VOLUME;
    audio_cmd.payload.set_volume.volume = volume;
    return app_tasking_send_audio_command(&audio_cmd, app_api_queue_timeout_ms());
}

esp_err_t app_api_set_feature_flags(bool network_enabled, bool mqtt_enabled, bool ai_enabled, bool web_enabled)
{
    return config_manager_set_feature_flags(network_enabled, mqtt_enabled, ai_enabled, web_enabled);
}

esp_err_t app_api_set_aura_gap_ms(uint32_t gap_ms)
{
    return config_manager_set_aura_gap_ms(gap_ms);
}

esp_err_t app_api_set_aura_directories(const char *intro_dir, const char *response_dir)
{
    return config_manager_set_aura_directories(intro_dir, response_dir);
}

esp_err_t app_api_set_prophecy_timing(uint32_t gap12_ms, uint32_t gap23_ms, uint32_t gap34_ms, uint32_t leadin_wait_ms)
{
    return config_manager_set_prophecy_timing(gap12_ms, gap23_ms, gap34_ms, leadin_wait_ms);
}

esp_err_t app_api_set_hybrid_params(uint16_t reject_threshold_permille, uint32_t mic_capture_ms)
{
    return config_manager_set_hybrid_params(reject_threshold_permille, mic_capture_ms);
}

esp_err_t app_api_set_hybrid_unknown_retry_max(uint8_t unknown_retry_max)
{
    return config_manager_set_hybrid_unknown_retry_max(unknown_retry_max);
}

esp_err_t app_api_set_prophecy_background(uint16_t gain_permille, uint32_t fade_in_ms, uint32_t fade_out_ms)
{
    return config_manager_set_prophecy_background(gain_permille, fade_in_ms, fade_out_ms);
}

esp_err_t app_api_set_hybrid_effect(uint32_t idle_scene_id,
                                    uint32_t talk_scene_id,
                                    uint8_t speed,
                                    uint8_t intensity,
                                    uint8_t scale,
                                    bool apply_now)
{
    esp_err_t err = config_manager_set_hybrid_effect(idle_scene_id, talk_scene_id, speed, intensity, scale);
    if (err != ESP_OK || !apply_now) {
        return err;
    }
    led_command_t led_cmd = { 0 };
    led_cmd.id = LED_CMD_SET_EFFECT_PARAMS;
    led_cmd.payload.set_effect_params.speed = speed;
    led_cmd.payload.set_effect_params.intensity = intensity;
    led_cmd.payload.set_effect_params.scale = scale;
    return app_tasking_send_led_command(&led_cmd, app_api_queue_timeout_ms());
}

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
                                            bool apply_now)
{
    esp_err_t err = config_manager_set_hybrid_effect_palette(palette_mode,
                                                             color1_r,
                                                             color1_g,
                                                             color1_b,
                                                             color2_r,
                                                             color2_g,
                                                             color2_b,
                                                             color3_r,
                                                             color3_g,
                                                             color3_b);
    if (err != ESP_OK || !apply_now) {
        return err;
    }
    led_command_t led_cmd = { 0 };
    led_cmd.id = LED_CMD_SET_EFFECT_PALETTE;
    led_cmd.payload.set_effect_palette.mode = palette_mode;
    led_cmd.payload.set_effect_palette.c1_r = color1_r;
    led_cmd.payload.set_effect_palette.c1_g = color1_g;
    led_cmd.payload.set_effect_palette.c1_b = color1_b;
    led_cmd.payload.set_effect_palette.c2_r = color2_r;
    led_cmd.payload.set_effect_palette.c2_g = color2_g;
    led_cmd.payload.set_effect_palette.c2_b = color2_b;
    led_cmd.payload.set_effect_palette.c3_r = color3_r;
    led_cmd.payload.set_effect_palette.c3_g = color3_g;
    led_cmd.payload.set_effect_palette.c3_b = color3_b;
    return app_tasking_send_led_command(&led_cmd, app_api_queue_timeout_ms());
}

esp_err_t app_api_get_offline_submode(orb_offline_submode_t *out_submode)
{
    return config_manager_get_offline_submode(out_submode);
}

esp_err_t app_api_set_offline_submode(orb_offline_submode_t submode)
{
    return config_manager_set_offline_submode(submode);
}

const char *app_api_offline_submode_to_str(orb_offline_submode_t submode)
{
    return config_manager_offline_submode_to_str(submode);
}

bool app_api_parse_offline_submode(const char *text, orb_offline_submode_t *out_submode)
{
    return config_manager_parse_offline_submode(text, out_submode);
}

esp_err_t app_api_request_offline_lottery_start(void)
{
    return config_manager_request_offline_lottery_start();
}

esp_err_t app_api_set_offline_lottery_settings(uint8_t team_count,
                                               uint16_t participants_total,
                                               const orb_lottery_team_config_t *teams,
                                               size_t teams_count,
                                               uint8_t finish_source,
                                               const char *finish_value)
{
    return config_manager_set_offline_lottery_settings(team_count,
                                                       participants_total,
                                                       teams,
                                                       teams_count,
                                                       finish_source,
                                                       finish_value);
}

esp_err_t app_api_audio_stop(void)
{
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_STOP;
    return app_tasking_send_audio_command(&audio_cmd, app_api_queue_timeout_ms());
}
