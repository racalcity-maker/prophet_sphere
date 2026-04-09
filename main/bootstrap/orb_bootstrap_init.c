#include "orb_bootstrap_internal.h"

#include "app_tasking.h"
#include "bsp_board.h"
#include "config_manager.h"
#include "esp_err.h"
#include "esp_log.h"
#include "log_tags.h"
#include "nvs_flash.h"
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

esp_err_t orb_bootstrap_init_core(void)
{
    esp_err_t err = init_nvs_storage();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = bsp_board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_board_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = config_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_manager_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = app_tasking_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_tasking_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = service_runtime_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "service_runtime_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rest_api_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rest_api_init failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
