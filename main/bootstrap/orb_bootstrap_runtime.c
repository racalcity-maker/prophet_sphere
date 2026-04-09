#include "orb_bootstrap_internal.h"

#include "app_tasking.h"
#include "esp_err.h"
#include "esp_log.h"
#include "log_tags.h"
#include "mode_manager.h"
#include "ota_service.h"
#include "orb_mode_runtime_policy.h"
#include "service_runtime.h"

static const char *TAG = LOG_TAG_APP_MAIN;

esp_err_t orb_bootstrap_init_runtime(void)
{
    esp_err_t err = mode_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode_manager_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mode_manager_register_builtin_modes();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode_manager_register_builtin_modes failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mode_manager_set_runtime_apply_hook(orb_mode_runtime_apply);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode_manager_set_runtime_apply_hook failed: %s", esp_err_to_name(err));
        return err;
    }

    err = app_tasking_start_app_control_task();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_tasking_start_app_control_task failed: %s", esp_err_to_name(err));
        return err;
    }

    err = ota_service_mark_boot_success();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota_service_mark_boot_success failed: %s", esp_err_to_name(err));
    }

    err = mode_manager_activate_default_mode();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "default mode activation failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}
