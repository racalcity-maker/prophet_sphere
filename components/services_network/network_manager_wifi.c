#include "network_manager_internal.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "log_tags.h"
#include "network_event_handlers.h"

static const char *TAG = LOG_TAG_NETWORK;

esp_err_t network_manager_init_wifi_core_locked(void)
{
    if (s_ctx.initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }

    if (s_ctx.netif_sta == NULL) {
        s_ctx.netif_sta = esp_netif_create_default_wifi_sta();
        if (s_ctx.netif_sta == NULL) {
            return ESP_FAIL;
        }
    }
    if (s_ctx.netif_ap == NULL) {
        s_ctx.netif_ap = esp_netif_create_default_wifi_ap();
        if (s_ctx.netif_ap == NULL) {
            return ESP_FAIL;
        }
    }
    if (CONFIG_ORB_NETWORK_HOSTNAME[0] != '\0') {
        (void)esp_netif_set_hostname(s_ctx.netif_sta, CONFIG_ORB_NETWORK_HOSTNAME);
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
#if CONFIG_ORB_NETWORK_DISABLE_WIFI_PS
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "esp_wifi_set_ps failed");
#endif

    const network_event_sink_t sink = {
        .on_sta_start = network_manager_on_sta_start,
        .on_sta_connected = network_manager_on_sta_connected,
        .on_sta_disconnected = network_manager_on_sta_disconnected,
        .on_ap_start = network_manager_on_ap_start,
        .on_ap_stop = network_manager_on_ap_stop,
        .on_sta_got_ip = network_manager_on_sta_got_ip,
    };
    network_event_handlers_init(&sink, NULL);

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            network_wifi_event_handler,
                                                            NULL,
                                                            &s_ctx.wifi_handler),
                        TAG,
                        "wifi handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            network_ip_event_handler,
                                                            NULL,
                                                            &s_ctx.ip_handler),
                        TAG,
                        "ip handler register failed");

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "network core initialized");
    return ESP_OK;
}

esp_err_t network_manager_stop_wifi_locked(void)
{
    s_sta_auto_reconnect = false;

    if (!s_ctx.started) {
        s_ctx.starting = false;
        s_ctx.active_profile = NETWORK_PROFILE_NONE;
        s_ctx.sta_retry_count = 0U;
        s_ctx.sta_ssid[0] = '\0';
        s_ctx.sta_ip[0] = '\0';
        s_ctx.ap_ip[0] = '\0';
        network_manager_update_link_state_locked("service_stopped");
        return ESP_OK;
    }

    if (s_ctx.active_profile == NETWORK_PROFILE_STA || s_ctx.active_profile == NETWORK_PROFILE_APSTA) {
        (void)esp_wifi_disconnect();
    }
    (void)esp_wifi_stop();
    if (s_ctx.event_group != NULL) {
        xEventGroupClearBits(s_ctx.event_group, NET_BIT_AP_STARTED | NET_BIT_STA_GOT_IP);
    }

    s_ctx.started = false;
    s_ctx.starting = false;
    s_ctx.active_profile = NETWORK_PROFILE_NONE;
    s_ctx.sta_retry_count = 0U;
    s_ctx.sta_ssid[0] = '\0';
    s_ctx.sta_ip[0] = '\0';
    s_ctx.ap_ip[0] = '\0';
    network_manager_update_link_state_locked("service_stopped");
    return ESP_OK;
}

esp_err_t network_manager_start_softap_locked(void)
{
    wifi_config_t ap_cfg;
    network_manager_fill_ap_config(&ap_cfg);

    if (s_ctx.event_group != NULL) {
        xEventGroupClearBits(s_ctx.event_group, NET_BIT_AP_STARTED | NET_BIT_STA_GOT_IP);
    }
    s_sta_auto_reconnect = false;
    s_ctx.sta_retry_count = 0U;
    s_ctx.sta_ssid[0] = '\0';
    s_ctx.sta_ip[0] = '\0';
    s_ctx.ap_ip[0] = '\0';

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start AP failed");

    s_ctx.started = true;
    s_ctx.starting = true;
    s_ctx.active_profile = NETWORK_PROFILE_SOFTAP;
    network_manager_update_link_state_locked("softap_start_requested");
    ESP_LOGI(TAG, "wifi start requested profile=softap ssid=%s", CONFIG_ORB_NETWORK_AP_SSID);
    return ESP_OK;
}

esp_err_t network_manager_start_sta_locked(void)
{
    char sta_ssid[33] = { 0 };
    char sta_password[65] = { 0 };
    network_manager_load_sta_credentials(sta_ssid, sizeof(sta_ssid), sta_password, sizeof(sta_password));
    if (!network_manager_sta_credentials_present(sta_ssid)) {
        ESP_LOGW(TAG, "STA profile selected but ssid is empty");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t sta_cfg;
    network_manager_fill_sta_config(&sta_cfg, sta_ssid, sta_password);

    if (s_ctx.event_group != NULL) {
        xEventGroupClearBits(s_ctx.event_group, NET_BIT_AP_STARTED | NET_BIT_STA_GOT_IP);
    }
    s_ctx.sta_retry_count = 0U;
    s_sta_auto_reconnect = true;
    strlcpy(s_ctx.sta_ssid, sta_ssid, sizeof(s_ctx.sta_ssid));
    s_ctx.sta_ip[0] = '\0';
    s_ctx.ap_ip[0] = '\0';

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set STA config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start STA failed");

    s_ctx.started = true;
    s_ctx.starting = true;
    s_ctx.active_profile = NETWORK_PROFILE_STA;
    network_manager_update_link_state_locked("sta_start_requested");
    ESP_LOGI(TAG, "wifi start requested profile=sta ssid=%s", s_ctx.sta_ssid);
    return ESP_OK;
}

esp_err_t network_manager_start_apsta_locked(void)
{
    char sta_ssid[33] = { 0 };
    char sta_password[65] = { 0 };
    network_manager_load_sta_credentials(sta_ssid, sizeof(sta_ssid), sta_password, sizeof(sta_password));

    wifi_config_t ap_cfg;
    wifi_config_t sta_cfg;
    network_manager_fill_ap_config(&ap_cfg);
    network_manager_fill_sta_config(&sta_cfg, sta_ssid, sta_password);

    if (s_ctx.event_group != NULL) {
        xEventGroupClearBits(s_ctx.event_group, NET_BIT_AP_STARTED | NET_BIT_STA_GOT_IP);
    }
    s_ctx.sta_retry_count = 0U;
    s_sta_auto_reconnect = network_manager_sta_credentials_present(sta_ssid);
    strlcpy(s_ctx.sta_ssid, sta_ssid, sizeof(s_ctx.sta_ssid));
    s_ctx.sta_ip[0] = '\0';
    s_ctx.ap_ip[0] = '\0';

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set STA config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start APSTA failed");

    s_ctx.started = true;
    s_ctx.starting = true;
    s_ctx.active_profile = NETWORK_PROFILE_APSTA;
    network_manager_update_link_state_locked("apsta_start_requested");
    ESP_LOGI(TAG,
             "wifi start requested profile=apsta ap_ssid=%s sta_ssid=%s",
             CONFIG_ORB_NETWORK_AP_SSID,
             (s_ctx.sta_ssid[0] == '\0') ? "-" : s_ctx.sta_ssid);
    return ESP_OK;
}
