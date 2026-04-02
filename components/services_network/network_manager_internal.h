#ifndef NETWORK_MANAGER_INTERNAL_H
#define NETWORK_MANAGER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include "app_events.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "network_manager.h"

#ifndef CONFIG_ORB_NETWORK_EVENT_POST_TIMEOUT_MS
#define CONFIG_ORB_NETWORK_EVENT_POST_TIMEOUT_MS CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS
#endif
#ifndef CONFIG_ORB_NETWORK_STA_RETRY_MAX
#define CONFIG_ORB_NETWORK_STA_RETRY_MAX 5
#endif
#ifndef CONFIG_ORB_NETWORK_STA_SSID
#define CONFIG_ORB_NETWORK_STA_SSID ""
#endif
#ifndef CONFIG_ORB_NETWORK_STA_PASSWORD
#define CONFIG_ORB_NETWORK_STA_PASSWORD ""
#endif
#ifndef CONFIG_ORB_NETWORK_AP_SSID
#define CONFIG_ORB_NETWORK_AP_SSID "orb-offline"
#endif
#ifndef CONFIG_ORB_NETWORK_AP_PASSWORD
#define CONFIG_ORB_NETWORK_AP_PASSWORD ""
#endif
#ifndef CONFIG_ORB_NETWORK_AP_CHANNEL
#define CONFIG_ORB_NETWORK_AP_CHANNEL 6
#endif
#ifndef CONFIG_ORB_NETWORK_AP_MAX_CONNECTIONS
#define CONFIG_ORB_NETWORK_AP_MAX_CONNECTIONS 4
#endif
#ifndef CONFIG_ORB_NETWORK_AP_HIDE_SSID
#define CONFIG_ORB_NETWORK_AP_HIDE_SSID 0
#endif
#ifndef CONFIG_ORB_NETWORK_HOSTNAME
#define CONFIG_ORB_NETWORK_HOSTNAME ""
#endif

#define NET_BIT_STA_GOT_IP BIT0
#define NET_BIT_AP_STARTED BIT1

typedef struct {
    bool initialized;
    bool started;
    bool starting;
    bool network_up;
    bool force_reconfigure;
    network_profile_t desired_profile;
    network_profile_t active_profile;
    network_link_state_t link_state;
    uint8_t sta_retry_count;
    esp_netif_t *netif_sta;
    esp_netif_t *netif_ap;
    SemaphoreHandle_t state_mutex;
    EventGroupHandle_t event_group;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    char sta_ip[16];
    char ap_ip[16];
    char sta_ssid[33];
} network_ctx_t;

extern network_ctx_t s_ctx;
extern volatile bool s_sta_auto_reconnect;

TickType_t network_manager_lock_timeout_ticks(void);
esp_err_t network_manager_lock_state(void);
bool network_manager_lock_state_from_event(void);
void network_manager_unlock_state(void);

esp_err_t network_manager_post_network_event(app_event_id_t id);
void network_manager_refresh_sta_ip_locked(void);
void network_manager_refresh_ap_ip_locked(void);
void network_manager_update_link_state_locked(const char *reason);

void network_manager_load_sta_credentials(char *out_ssid,
                                          size_t ssid_len,
                                          char *out_password,
                                          size_t pass_len);
bool network_manager_sta_credentials_present(const char *ssid);
void network_manager_fill_ap_config(wifi_config_t *cfg);
void network_manager_fill_sta_config(wifi_config_t *cfg, const char *ssid, const char *password);

void network_manager_on_sta_start(void *ctx);
void network_manager_on_sta_connected(void *ctx);
void network_manager_on_sta_disconnected(void *ctx, uint8_t reason);
void network_manager_on_ap_start(void *ctx);
void network_manager_on_ap_stop(void *ctx);
void network_manager_on_sta_got_ip(void *ctx, const esp_ip4_addr_t *ip);

esp_err_t network_manager_init_wifi_core_locked(void);
esp_err_t network_manager_stop_wifi_locked(void);
esp_err_t network_manager_start_softap_locked(void);
esp_err_t network_manager_start_sta_locked(void);
esp_err_t network_manager_start_apsta_locked(void);

#endif
