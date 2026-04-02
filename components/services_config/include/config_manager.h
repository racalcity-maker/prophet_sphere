#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "config_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small shared-state manager guarded by a non-recursive mutex.
 * Runtime snapshot can be persisted in NVS when enabled in menuconfig.
 */
esp_err_t config_manager_init(void);
esp_err_t config_manager_get_snapshot(orb_runtime_config_t *snapshot);
esp_err_t config_manager_set_led_brightness(uint8_t brightness);
esp_err_t config_manager_set_audio_volume(uint8_t volume);
esp_err_t config_manager_set_feature_flags(bool network_enabled, bool mqtt_enabled, bool ai_enabled, bool web_enabled);
esp_err_t config_manager_set_offline_submode(orb_offline_submode_t submode);
esp_err_t config_manager_request_offline_lottery_start(void);
esp_err_t config_manager_set_aura_gap_ms(uint32_t gap_ms);
esp_err_t config_manager_set_aura_directories(const char *intro_dir, const char *response_dir);
esp_err_t config_manager_set_aura_selected_color(const char *color_name);
esp_err_t config_manager_set_prophecy_timing(uint32_t gap12_ms, uint32_t gap23_ms, uint32_t gap34_ms, uint32_t leadin_wait_ms);
esp_err_t config_manager_set_prophecy_background(uint16_t gain_permille, uint32_t fade_in_ms, uint32_t fade_out_ms);
esp_err_t config_manager_set_hybrid_params(uint16_t reject_threshold_permille, uint32_t mic_capture_ms);
esp_err_t config_manager_set_wifi_sta_credentials(const char *ssid, const char *password);
esp_err_t config_manager_has_persisted_wifi_sta_credentials(bool *has_credentials);
esp_err_t config_manager_save(void);
esp_err_t config_manager_reload(void);
esp_err_t config_manager_reset_to_defaults(bool persist);

const char *config_manager_offline_submode_to_str(orb_offline_submode_t submode);
bool config_manager_parse_offline_submode(const char *text, orb_offline_submode_t *out_submode);

#ifdef __cplusplus
}
#endif

#endif
