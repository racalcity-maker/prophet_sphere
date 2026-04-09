#include "orb_bootstrap_internal.h"

#include <inttypes.h>
#include "bsp_board.h"
#include "config_manager.h"
#include "esp_err.h"
#include "esp_log.h"
#include "log_tags.h"
#include "memory_monitor.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_APP_MAIN;

void orb_bootstrap_log_startup_snapshot(void)
{
    orb_runtime_config_t cfg = { 0 };
    if (config_manager_get_snapshot(&cfg) == ESP_OK) {
        ESP_LOGI(TAG,
                 "startup config: offline_submode=%s aura_gap=%" PRIu32 "ms",
                 config_manager_offline_submode_to_str(cfg.offline_submode),
                 cfg.aura_gap_ms);
    }
    ESP_LOGI(TAG, "Orb startup complete: board=%s", bsp_board_name());
}

void orb_bootstrap_apply_runtime_log_profile(void)
{
    /* Keep touch logs quiet in normal operation. */
    esp_log_level_set(LOG_TAG_TOUCH, ESP_LOG_WARN);

#if CONFIG_ORB_TOUCH_ONLY_LOG_MODE
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(LOG_TAG_MODE_BUTTON, ESP_LOG_INFO);
    esp_log_level_set(LOG_TAG_SUBMODE_BUTTON, ESP_LOG_INFO);
    esp_log_level_set(LOG_TAG_MODE_MANAGER, ESP_LOG_INFO);
    esp_log_level_set(LOG_TAG_NETWORK, ESP_LOG_INFO);
    esp_log_level_set(LOG_TAG_SERVICE_RUNTIME, ESP_LOG_INFO);
    esp_log_level_set(LOG_TAG_WEB, ESP_LOG_INFO);
    esp_log_level_set(LOG_TAG_REST, ESP_LOG_INFO);
    esp_log_level_set(LOG_TAG_MEM_MON, ESP_LOG_INFO);
#endif
}

esp_err_t orb_bootstrap_start_memory_monitor(void)
{
    esp_err_t err = memory_monitor_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "memory_monitor_start failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}
