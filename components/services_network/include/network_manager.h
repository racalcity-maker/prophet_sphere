#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETWORK_PROFILE_NONE = 0,
    NETWORK_PROFILE_SOFTAP,
    NETWORK_PROFILE_STA,
    NETWORK_PROFILE_APSTA,
} network_profile_t;

typedef enum {
    NETWORK_LINK_STOPPED = 0,
    NETWORK_LINK_STARTING,
    NETWORK_LINK_CONNECTED,
    NETWORK_LINK_AP,
    NETWORK_LINK_DEGRADED,
} network_link_state_t;

typedef struct {
    bool initialized;
    bool started;
    bool network_up;
    network_link_state_t link_state;
    network_profile_t desired_profile;
    network_profile_t active_profile;
    char sta_ssid[33];
    char sta_ip[16];
    char ap_ip[16];
} network_status_t;

esp_err_t network_manager_init(void);
esp_err_t network_manager_start(void);
esp_err_t network_manager_stop(void);

esp_err_t network_manager_set_desired_profile(network_profile_t profile);
esp_err_t network_manager_get_status(network_status_t *status);
esp_err_t network_manager_apply_sta_credentials(const char *ssid, const char *password, bool persist);
const char *network_manager_profile_to_str(network_profile_t profile);
const char *network_manager_link_state_to_str(network_link_state_t state);

#ifdef __cplusplus
}
#endif

#endif
