#include <inttypes.h>
#include "app_tasking.h"
#include "bsp_board.h"
#include "bsp_mode_button.h"
#include "bsp_submode_button.h"
#include "config_manager.h"
#include "esp_log.h"
#include "log_tags.h"
#include "mode_manager.h"
#include "memory_monitor.h"
#include "nvs_flash.h"
#include "ota_service.h"
#include "rest_api.h"
#include "service_runtime.h"

static const char *TAG = LOG_TAG_APP_MAIN;

static esp_err_t init_nvs_storage(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void apply_runtime_log_profile(void)
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

void app_main(void)
{
    esp_err_t err;

    /* Startup order:
     * 1) NVS
     * 2) BSP and config manager
     * 3) Queue/tasking layer (queues + FSM)
     * 4) Service runtime initialization (shared-service lifecycle orchestrator)
     * 5) Mode manager + built-in mode registration + runtime hook binding
     * 6) app_control_task
     * 7) default mode activation by queue request (runtime applies service requirements)
     * 8) optional hardware mode/submode button tasks (active-low to GND)
     */
    err = init_nvs_storage();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    err = bsp_board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_board_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = config_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_manager_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_tasking_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_tasking_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = service_runtime_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "service_runtime_init failed: %s", esp_err_to_name(err));
        return;
    }
    err = rest_api_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rest_api_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = mode_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode_manager_init failed: %s", esp_err_to_name(err));
        return;
    }
    err = mode_manager_register_builtin_modes();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode_manager_register_builtin_modes failed: %s", esp_err_to_name(err));
        return;
    }
    err = mode_manager_set_runtime_apply_hook(service_runtime_apply_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode_manager_set_runtime_apply_hook failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_tasking_start_app_control_task();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_tasking_start_app_control_task failed: %s", esp_err_to_name(err));
        return;
    }

    err = ota_service_mark_boot_success();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota_service_mark_boot_success failed: %s", esp_err_to_name(err));
    }

    /* Always activate default mode through event queue path. */
    err = mode_manager_activate_default_mode();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "default mode activation failed: %s", esp_err_to_name(err));
    }

    err = bsp_mode_button_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_mode_button_start failed: %s", esp_err_to_name(err));
        return;
    }
    err = bsp_submode_button_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_submode_button_start failed: %s", esp_err_to_name(err));
        return;
    }

    orb_runtime_config_t cfg = { 0 };
    if (config_manager_get_snapshot(&cfg) == ESP_OK) {
        ESP_LOGI(TAG,
                 "startup config: offline_submode=%s aura_gap=%" PRIu32 "ms",
                 config_manager_offline_submode_to_str(cfg.offline_submode),
                 cfg.aura_gap_ms);
    }

    ESP_LOGI(TAG, "Orb startup complete: board=%s", bsp_board_name());
    apply_runtime_log_profile();

    err = memory_monitor_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "memory_monitor_start failed: %s", esp_err_to_name(err));
    }
}
