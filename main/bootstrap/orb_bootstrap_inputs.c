#include "orb_bootstrap_internal.h"

#include "app_events.h"
#include "app_tasking.h"
#include "bsp_mode_button.h"
#include "bsp_submode_button.h"
#include "esp_err.h"
#include "esp_log.h"
#include "log_tags.h"
#include "mode_manager.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = LOG_TAG_APP_MAIN;

static orb_mode_t next_mode_from(orb_mode_t current)
{
    switch (current) {
    case ORB_MODE_OFFLINE_SCRIPTED:
        return ORB_MODE_HYBRID_AI;
    case ORB_MODE_HYBRID_AI:
        return ORB_MODE_INSTALLATION_SLAVE;
    case ORB_MODE_INSTALLATION_SLAVE:
        return ORB_MODE_OFFLINE_SCRIPTED;
    case ORB_MODE_NONE:
    default:
        return ORB_MODE_OFFLINE_SCRIPTED;
    }
}

static void on_mode_button_pressed(void *user_ctx)
{
    (void)user_ctx;
    orb_mode_t current = mode_manager_get_current_mode();
    orb_mode_t target = next_mode_from(current);
    esp_err_t err = mode_manager_request_switch(target);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "mode button request failed: %d -> %d (%s)",
                 (int)current,
                 (int)target,
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "mode button: %d -> %d", (int)current, (int)target);
}

static void on_submode_button_pressed(void *user_ctx)
{
    (void)user_ctx;
    app_event_t event = { 0 };
    event.id = APP_EVENT_SUBMODE_BUTTON_REQUEST;
    event.source = APP_EVENT_SOURCE_CONTROL;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    esp_err_t err = app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "submode button request post failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "submode button pressed");
}

esp_err_t orb_bootstrap_bind_inputs(void)
{
    esp_err_t err = bsp_mode_button_set_pressed_callback(on_mode_button_pressed, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_mode_button_set_pressed_callback failed: %s", esp_err_to_name(err));
        return err;
    }

    err = bsp_submode_button_set_pressed_callback(on_submode_button_pressed, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_submode_button_set_pressed_callback failed: %s", esp_err_to_name(err));
        return err;
    }

    err = bsp_mode_button_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_mode_button_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = bsp_submode_button_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_submode_button_start failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
