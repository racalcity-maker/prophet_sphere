#include "network_manager_internal.h"

#include <stdio.h>
#include "esp_log.h"
#include "log_tags.h"
#include "lwip/ip4_addr.h"

static const char *TAG = LOG_TAG_NETWORK;

void network_manager_on_sta_start(void *ctx)
{
    (void)ctx;
    bool do_connect = false;
    if (!network_manager_lock_state_from_event()) {
        return;
    }
    if (s_sta_auto_reconnect) {
        s_ctx.sta_retry_count = 0U;
        do_connect = true;
    }
    network_manager_unlock_state();
    if (do_connect) {
        (void)esp_wifi_connect();
    }
}

void network_manager_on_sta_connected(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "STA connected to AP");
}

void network_manager_on_sta_disconnected(void *ctx, uint8_t reason)
{
    (void)ctx;
    bool should_reconnect = false;
    uint8_t retry = 0U;

    if (!network_manager_lock_state_from_event()) {
        return;
    }
    if (s_ctx.event_group != NULL) {
        xEventGroupClearBits(s_ctx.event_group, NET_BIT_STA_GOT_IP);
    }
    s_ctx.sta_ip[0] = '\0';
    s_ctx.starting = false;

    if (s_sta_auto_reconnect && s_ctx.sta_retry_count < (uint8_t)CONFIG_ORB_NETWORK_STA_RETRY_MAX) {
        s_ctx.sta_retry_count++;
        retry = s_ctx.sta_retry_count;
        should_reconnect = true;
    }
    network_manager_update_link_state_locked("sta_disconnected");
    network_manager_unlock_state();

    ESP_LOGW(TAG,
             "STA disconnected reason=%u retry=%u/%u",
             (unsigned)reason,
             (unsigned)retry,
             (unsigned)CONFIG_ORB_NETWORK_STA_RETRY_MAX);

    if (should_reconnect) {
        (void)esp_wifi_connect();
    }
}

void network_manager_on_ap_start(void *ctx)
{
    (void)ctx;
    if (!network_manager_lock_state_from_event()) {
        return;
    }
    if (s_ctx.event_group != NULL) {
        xEventGroupSetBits(s_ctx.event_group, NET_BIT_AP_STARTED);
    }
    s_ctx.starting = false;
    network_manager_refresh_ap_ip_locked();
    network_manager_update_link_state_locked("ap_started");
    ESP_LOGI(TAG,
             "SoftAP up ssid=%s ip=%s",
             CONFIG_ORB_NETWORK_AP_SSID,
             (s_ctx.ap_ip[0] == '\0') ? "n/a" : s_ctx.ap_ip);
    network_manager_unlock_state();
}

void network_manager_on_ap_stop(void *ctx)
{
    (void)ctx;
    if (!network_manager_lock_state_from_event()) {
        return;
    }
    if (s_ctx.event_group != NULL) {
        xEventGroupClearBits(s_ctx.event_group, NET_BIT_AP_STARTED);
    }
    s_ctx.ap_ip[0] = '\0';
    network_manager_update_link_state_locked("ap_stopped");
    network_manager_unlock_state();
}

void network_manager_on_sta_got_ip(void *ctx, const esp_ip4_addr_t *ip)
{
    (void)ctx;
    if (!network_manager_lock_state_from_event()) {
        return;
    }
    if (ip != NULL) {
        snprintf(s_ctx.sta_ip, sizeof(s_ctx.sta_ip), IPSTR, IP2STR(ip));
    } else {
        network_manager_refresh_sta_ip_locked();
    }
    s_ctx.starting = false;
    s_ctx.sta_retry_count = 0U;
    if (s_ctx.event_group != NULL) {
        xEventGroupSetBits(s_ctx.event_group, NET_BIT_STA_GOT_IP);
    }
    network_manager_update_link_state_locked("sta_got_ip");
    ESP_LOGI(TAG,
             "STA got IP=%s (ssid=%s)",
             (s_ctx.sta_ip[0] == '\0') ? "n/a" : s_ctx.sta_ip,
             (s_ctx.sta_ssid[0] == '\0') ? "-" : s_ctx.sta_ssid);
    network_manager_unlock_state();
}
