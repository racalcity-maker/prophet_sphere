#include "network_manager.h"

#include <string.h>
#include "config_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "log_tags.h"
#include "network_manager_internal.h"
#include "network_policy.h"
#include "sdkconfig.h"
#include "service_lifecycle_guard.h"

static const char *TAG = LOG_TAG_NETWORK;

const char *network_manager_profile_to_str(network_profile_t profile)
{
    switch (profile) {
    case NETWORK_PROFILE_NONE:
        return "none";
    case NETWORK_PROFILE_SOFTAP:
        return "softap";
    case NETWORK_PROFILE_STA:
        return "sta";
    case NETWORK_PROFILE_APSTA:
        return "apsta";
    default:
        return "unknown";
    }
}

const char *network_manager_link_state_to_str(network_link_state_t state)
{
    switch (state) {
    case NETWORK_LINK_STOPPED:
        return "stopped";
    case NETWORK_LINK_STARTING:
        return "starting";
    case NETWORK_LINK_CONNECTED:
        return "connected";
    case NETWORK_LINK_AP:
        return "ap";
    case NETWORK_LINK_DEGRADED:
        return "degraded";
    default:
        return "unknown";
    }
}

esp_err_t network_manager_init(void)
{
    if (!CONFIG_ORB_ENABLE_NETWORK) {
        ESP_LOGW(TAG, "network disabled");
        return ESP_OK;
    }

    if (s_ctx.state_mutex == NULL) {
        s_ctx.state_mutex = xSemaphoreCreateMutex();
        if (s_ctx.state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_ctx.event_group == NULL) {
        s_ctx.event_group = xEventGroupCreate();
        if (s_ctx.event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(network_manager_lock_state(), TAG, "network lock failed");
    if (!s_ctx.initialized) {
        s_ctx.desired_profile = NETWORK_PROFILE_NONE;
        s_ctx.active_profile = NETWORK_PROFILE_NONE;
        s_ctx.link_state = NETWORK_LINK_STOPPED;
        s_ctx.network_up = false;
        s_ctx.force_reconfigure = false;
        s_ctx.started = false;
        s_ctx.starting = false;
        s_ctx.sta_retry_count = 0U;
        s_ctx.sta_ssid[0] = '\0';
        s_ctx.sta_ip[0] = '\0';
        s_ctx.ap_ip[0] = '\0';
        s_sta_auto_reconnect = false;
        esp_err_t err = network_manager_init_wifi_core_locked();
        if (err != ESP_OK) {
            network_manager_unlock_state();
            return err;
        }
        ESP_LOGI(TAG, "network manager initialized");
    }
    network_manager_unlock_state();

    return ESP_OK;
}

esp_err_t network_manager_set_desired_profile(network_profile_t profile)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "network profile set denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CONFIG_ORB_ENABLE_NETWORK) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(network_manager_init(), TAG, "network init failed");

    ESP_RETURN_ON_ERROR(network_manager_lock_state(), TAG, "network lock failed");
    network_profile_t previous = s_ctx.desired_profile;
    s_ctx.desired_profile = profile;
    network_manager_unlock_state();

    if (previous != profile) {
        ESP_LOGI(TAG,
                 "network desired profile: %s -> %s",
                 network_manager_profile_to_str(previous),
                 network_manager_profile_to_str(profile));
    }
    return ESP_OK;
}

esp_err_t network_manager_start(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "network start denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CONFIG_ORB_ENABLE_NETWORK) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(network_manager_init(), TAG, "network init failed");
    ESP_RETURN_ON_ERROR(network_manager_lock_state(), TAG, "network lock failed");

    network_profile_t desired = s_ctx.desired_profile;
    network_profile_t effective = network_policy_resolve_effective_profile(desired);

    if (effective == NETWORK_PROFILE_NONE) {
        esp_err_t stop_err = network_manager_stop_wifi_locked();
        s_ctx.force_reconfigure = false;
        network_manager_unlock_state();
        return stop_err;
    }

    if (s_ctx.started && s_ctx.active_profile == effective && !s_ctx.force_reconfigure) {
        network_manager_unlock_state();
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(network_manager_stop_wifi_locked(), TAG, "wifi stop before reconfigure failed");

    if (desired != effective) {
        ESP_LOGI(TAG,
                 "network start effective profile: requested=%s effective=%s",
                 network_manager_profile_to_str(desired),
                 network_manager_profile_to_str(effective));
    }

    esp_err_t err = ESP_OK;
    if (effective == NETWORK_PROFILE_SOFTAP) {
        err = network_manager_start_softap_locked();
    } else if (effective == NETWORK_PROFILE_STA) {
        err = network_manager_start_sta_locked();
    } else if (effective == NETWORK_PROFILE_APSTA) {
        err = network_manager_start_apsta_locked();
    } else {
        err = ESP_ERR_INVALID_ARG;
    }

    if (err != ESP_OK) {
        s_ctx.started = false;
        s_ctx.starting = false;
        s_ctx.active_profile = NETWORK_PROFILE_NONE;
        network_manager_update_link_state_locked("start_failed");
    }
    s_ctx.force_reconfigure = false;
    network_manager_unlock_state();
    return err;
}

esp_err_t network_manager_stop(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "network stop denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CONFIG_ORB_ENABLE_NETWORK) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(network_manager_init(), TAG, "network init failed");
    ESP_RETURN_ON_ERROR(network_manager_lock_state(), TAG, "network lock failed");
    esp_err_t err = network_manager_stop_wifi_locked();
    s_ctx.force_reconfigure = false;
    network_manager_unlock_state();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "network stopped");
    }
    return err;
}

esp_err_t network_manager_get_status(network_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));

    if (!CONFIG_ORB_ENABLE_NETWORK) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(network_manager_init(), TAG, "network init failed");
    ESP_RETURN_ON_ERROR(network_manager_lock_state(), TAG, "network lock failed");

    status->initialized = s_ctx.initialized;
    status->started = s_ctx.started;
    status->network_up = s_ctx.network_up;
    status->link_state = s_ctx.link_state;
    status->desired_profile = s_ctx.desired_profile;
    status->active_profile = s_ctx.active_profile;
    strlcpy(status->sta_ssid, s_ctx.sta_ssid, sizeof(status->sta_ssid));
    strlcpy(status->sta_ip, s_ctx.sta_ip, sizeof(status->sta_ip));
    strlcpy(status->ap_ip, s_ctx.ap_ip, sizeof(status->ap_ip));

    network_manager_unlock_state();
    return ESP_OK;
}

esp_err_t network_manager_apply_sta_credentials(const char *ssid, const char *password, bool persist)
{
    if (ssid == NULL || ssid[0] == '\0' || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(config_manager_set_wifi_sta_credentials(ssid, password),
                        TAG,
                        "save sta credentials failed");
    if (persist) {
        (void)config_manager_save();
    }

    ESP_RETURN_ON_ERROR(network_manager_init(), TAG, "network init failed");
    ESP_RETURN_ON_ERROR(network_manager_lock_state(), TAG, "network lock failed");
    s_ctx.force_reconfigure = true;
    network_manager_unlock_state();

    return ESP_OK;
}
