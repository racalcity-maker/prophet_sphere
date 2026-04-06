#include "network_manager_internal.h"

#include <string.h>
#include "config_manager.h"

void network_manager_load_sta_credentials(char *out_ssid,
                                          size_t ssid_len,
                                          char *out_password,
                                          size_t pass_len)
{
    if (out_ssid != NULL && ssid_len > 0U) {
        out_ssid[0] = '\0';
    }
    if (out_password != NULL && pass_len > 0U) {
        out_password[0] = '\0';
    }

    orb_runtime_config_t cfg = { 0 };
    if (config_manager_get_snapshot(&cfg) == ESP_OK) {
        if (out_ssid != NULL && ssid_len > 0U) {
            strlcpy(out_ssid, cfg.wifi_sta_ssid, ssid_len);
        }
        if (out_password != NULL && pass_len > 0U) {
            strlcpy(out_password, cfg.wifi_sta_password, pass_len);
        }
    }

    if (out_ssid != NULL && out_ssid[0] == '\0') {
        strlcpy(out_ssid, CONFIG_ORB_NETWORK_STA_SSID, ssid_len);
    }
    if (out_password != NULL && out_password[0] == '\0') {
        strlcpy(out_password, CONFIG_ORB_NETWORK_STA_PASSWORD, pass_len);
    }
}

bool network_manager_sta_credentials_present(const char *ssid)
{
    return (ssid != NULL && ssid[0] != '\0');
}

void network_manager_fill_ap_config(wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy((char *)cfg->ap.ssid, CONFIG_ORB_NETWORK_AP_SSID, sizeof(cfg->ap.ssid));
    cfg->ap.ssid_len = (uint8_t)strlen(CONFIG_ORB_NETWORK_AP_SSID);
    strlcpy((char *)cfg->ap.password, CONFIG_ORB_NETWORK_AP_PASSWORD, sizeof(cfg->ap.password));
    cfg->ap.channel = (uint8_t)CONFIG_ORB_NETWORK_AP_CHANNEL;
    cfg->ap.max_connection = (uint8_t)CONFIG_ORB_NETWORK_AP_MAX_CONNECTIONS;
    cfg->ap.ssid_hidden = CONFIG_ORB_NETWORK_AP_HIDE_SSID ? 1U : 0U;
    cfg->ap.pmf_cfg.capable = true;
    cfg->ap.pmf_cfg.required = false;
    cfg->ap.authmode = (strlen(CONFIG_ORB_NETWORK_AP_PASSWORD) >= 8U) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (cfg->ap.authmode == WIFI_AUTH_OPEN) {
        cfg->ap.password[0] = '\0';
    }
}

void network_manager_fill_sta_config(wifi_config_t *cfg, const char *ssid, const char *password)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy((char *)cfg->sta.ssid, ssid, sizeof(cfg->sta.ssid));
    strlcpy((char *)cfg->sta.password, password, sizeof(cfg->sta.password));
    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.failure_retry_cnt = (uint8_t)CONFIG_ORB_NETWORK_STA_RETRY_MAX;
    cfg->sta.pmf_cfg.capable = true;
    cfg->sta.pmf_cfg.required = false;
    cfg->sta.threshold.authmode = (strlen(password) == 0U) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
}
