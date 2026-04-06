#include "network_manager_internal.h"

#include <stdio.h>
#include "app_tasking.h"
#include "esp_log.h"
#include "log_tags.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_NETWORK;

network_ctx_t s_ctx;
volatile bool s_sta_auto_reconnect;

TickType_t network_manager_lock_timeout_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    return (ticks > 0) ? ticks : 1;
}

esp_err_t network_manager_lock_state(void)
{
    if (s_ctx.state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_ctx.state_mutex, network_manager_lock_timeout_ticks()) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool network_manager_lock_state_from_event(void)
{
    if (s_ctx.state_mutex == NULL) {
        return false;
    }
    return (xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY) == pdTRUE);
}

void network_manager_unlock_state(void)
{
    if (s_ctx.state_mutex != NULL) {
        xSemaphoreGive(s_ctx.state_mutex);
    }
}

esp_err_t network_manager_post_network_event(app_event_id_t id)
{
    app_event_t event = { 0 };
    event.id = id;
    event.source = APP_EVENT_SOURCE_NETWORK;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return app_tasking_post_event(&event, CONFIG_ORB_NETWORK_EVENT_POST_TIMEOUT_MS);
}

void network_manager_refresh_sta_ip_locked(void)
{
    if (s_ctx.netif_sta == NULL) {
        s_ctx.sta_ip[0] = '\0';
        return;
    }
    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_get_ip_info(s_ctx.netif_sta, &ip) == ESP_OK) {
        snprintf(s_ctx.sta_ip, sizeof(s_ctx.sta_ip), IPSTR, IP2STR(&ip.ip));
    } else {
        s_ctx.sta_ip[0] = '\0';
    }
}

void network_manager_refresh_ap_ip_locked(void)
{
    if (s_ctx.netif_ap == NULL) {
        s_ctx.ap_ip[0] = '\0';
        return;
    }
    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_get_ip_info(s_ctx.netif_ap, &ip) == ESP_OK) {
        snprintf(s_ctx.ap_ip, sizeof(s_ctx.ap_ip), IPSTR, IP2STR(&ip.ip));
    } else {
        s_ctx.ap_ip[0] = '\0';
    }
}

void network_manager_update_link_state_locked(const char *reason)
{
    EventBits_t bits = (s_ctx.event_group != NULL) ? xEventGroupGetBits(s_ctx.event_group) : 0U;
    bool has_sta_ip = ((bits & NET_BIT_STA_GOT_IP) != 0U);
    bool ap_started = ((bits & NET_BIT_AP_STARTED) != 0U);

    network_link_state_t next_state = NETWORK_LINK_STOPPED;
    if (!s_ctx.started || s_ctx.active_profile == NETWORK_PROFILE_NONE) {
        next_state = NETWORK_LINK_STOPPED;
    } else if (s_ctx.starting) {
        next_state = NETWORK_LINK_STARTING;
    } else if (has_sta_ip) {
        next_state = NETWORK_LINK_CONNECTED;
    } else if (ap_started) {
        next_state = NETWORK_LINK_AP;
    } else {
        next_state = NETWORK_LINK_DEGRADED;
    }

    if (next_state != s_ctx.link_state) {
        ESP_LOGI(TAG,
                 "link state: %s -> %s (%s)",
                 network_manager_link_state_to_str(s_ctx.link_state),
                 network_manager_link_state_to_str(next_state),
                 reason == NULL ? "n/a" : reason);
        s_ctx.link_state = next_state;
    }

    bool up = has_sta_ip || ap_started;
    if (up != s_ctx.network_up) {
        s_ctx.network_up = up;
        app_event_id_t id = up ? APP_EVENT_NETWORK_UP : APP_EVENT_NETWORK_DOWN;
        esp_err_t err = network_manager_post_network_event(id);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "failed posting %s (%s): %s",
                     app_event_id_to_str(id),
                     reason == NULL ? "n/a" : reason,
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "%s (%s)", app_event_id_to_str(id), reason == NULL ? "n/a" : reason);
        }
    }
}
