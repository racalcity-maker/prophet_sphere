#ifndef CONFIG_STORE_NVS_H
#define CONFIG_STORE_NVS_H

#include <stdbool.h>
#include "config_schema.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NVS-backed persistence for orb_runtime_config_t.
 * This is an internal helper for config_manager.
 */
esp_err_t config_store_nvs_load(orb_runtime_config_t *cfg, bool *loaded);
esp_err_t config_store_nvs_save(const orb_runtime_config_t *cfg);
esp_err_t config_store_nvs_has_wifi_sta_credentials(bool *has_credentials);

#ifdef __cplusplus
}
#endif

#endif
