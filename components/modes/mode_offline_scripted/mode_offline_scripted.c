#include "app_mode.h"

#include "esp_log.h"
#include "log_tags.h"
#include "offline_submode.h"

static const char *TAG = LOG_TAG_MODE_OFFLINE;

static esp_err_t mode_init(void)
{
    esp_err_t err = offline_submode_router_init();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}

static esp_err_t mode_enter(void)
{
    esp_err_t err = offline_submode_router_enter();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "enter");
    return ESP_OK;
}

static esp_err_t mode_exit(void)
{
    esp_err_t err = offline_submode_router_exit();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "exit");
    return ESP_OK;
}

static esp_err_t mode_handle_event(const app_mode_event_t *event, app_mode_action_t *action)
{
    if (event == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return offline_submode_router_handle_event(event, action);
}

const app_mode_t *mode_offline_scripted_get(void)
{
    static const app_mode_t mode = {
        .id = ORB_MODE_OFFLINE_SCRIPTED,
        .name = "offline_scripted",
        .init = mode_init,
        .enter = mode_enter,
        .exit = mode_exit,
        .handle_event = mode_handle_event,
    };
    return &mode;
}
