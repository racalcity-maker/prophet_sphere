#include "app_mode.h"

#include <stdbool.h>
#include "esp_log.h"
#include "log_tags.h"
#include "orb_led_scenes.h"

static const char *TAG = LOG_TAG_MODE_INSTALL;
static uint8_t s_pressed_mask;
static bool s_loopback_active;

static esp_err_t mode_init(void)
{
    s_pressed_mask = 0U;
    s_loopback_active = false;
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}

static esp_err_t mode_enter(void)
{
    s_pressed_mask = 0U;
    s_loopback_active = false;
    ESP_LOGI(TAG, "enter");
    return ESP_OK;
}

static esp_err_t mode_exit(void)
{
    s_pressed_mask = 0U;
    s_loopback_active = false;
    ESP_LOGI(TAG, "exit");
    return ESP_OK;
}

static esp_err_t mode_handle_event(const app_mode_event_t *event, app_mode_action_t *action)
{
    if (event == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *action = (app_mode_action_t){ 0 };

    switch (event->id) {
    case APP_MODE_EVENT_TOUCH_DOWN: {
        uint8_t zone = (uint8_t)(event->value & 0xFFU);
        if (zone < 8U) {
            s_pressed_mask |= (uint8_t)(1U << zone);
        }
        if (!s_loopback_active) {
            action->id = APP_MODE_ACTION_MIC_LOOPBACK_START;
            action->led.scene_id = ORB_LED_SCENE_ID_SPARKLE;
            s_loopback_active = true;
            ESP_LOGI(TAG, "touch down zone=%u -> loopback start", (unsigned)zone);
        }
        break;
    }
    case APP_MODE_EVENT_TOUCH_UP: {
        uint8_t zone = (uint8_t)(event->value & 0xFFU);
        if (zone < 8U) {
            s_pressed_mask &= (uint8_t)~(1U << zone);
        }
        if (s_loopback_active && s_pressed_mask == 0U) {
            action->id = APP_MODE_ACTION_MIC_LOOPBACK_STOP;
            action->led.scene_id = ORB_LED_SCENE_ID_COLOR_WAVE;
            s_loopback_active = false;
            ESP_LOGI(TAG, "touch up zone=%u -> loopback stop", (unsigned)zone);
        }
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

const app_mode_t *mode_installation_get(void)
{
    static const app_mode_t mode = {
        .id = ORB_MODE_INSTALLATION_SLAVE,
        .name = "installation_slave",
        .init = mode_init,
        .enter = mode_enter,
        .exit = mode_exit,
        .handle_event = mode_handle_event,
    };
    return &mode;
}
