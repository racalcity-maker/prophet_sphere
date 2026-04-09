#include "network_policy.h"

#include <string.h>
#include "config_manager.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_NETWORK_STA_SSID
#define CONFIG_ORB_NETWORK_STA_SSID ""
#endif

static bool sta_credentials_present(const char *ssid)
{
    return (ssid != NULL && ssid[0] != '\0');
}

static void load_effective_sta_ssid(char *out_ssid, size_t ssid_len)
{
    if (out_ssid == NULL || ssid_len == 0U) {
        return;
    }
    out_ssid[0] = '\0';

    (void)config_manager_get_wifi_sta_credentials(out_ssid, ssid_len, NULL, 0U);
    if (out_ssid[0] == '\0') {
        strlcpy(out_ssid, CONFIG_ORB_NETWORK_STA_SSID, ssid_len);
    }
}

network_profile_t network_policy_resolve_effective_profile(network_profile_t desired_profile)
{
    if (desired_profile != NETWORK_PROFILE_APSTA) {
        return desired_profile;
    }

#if CONFIG_ORB_NETWORK_APSTA_SMART_FROM_NVS
    bool has_nvs_sta_credentials = false;
    if (config_manager_has_persisted_wifi_sta_credentials(&has_nvs_sta_credentials) == ESP_OK &&
        has_nvs_sta_credentials) {
        return NETWORK_PROFILE_STA;
    }

    char effective_ssid[33] = { 0 };
    load_effective_sta_ssid(effective_ssid, sizeof(effective_ssid));
    if (sta_credentials_present(effective_ssid)) {
        return NETWORK_PROFILE_STA;
    }
#endif

    return desired_profile;
}
