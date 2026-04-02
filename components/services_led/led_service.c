#include "led_service.h"

#include "sdkconfig.h"
#include "app_tasking.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_output_ws2812.h"
#include "led_task.h"
#include "log_tags.h"
#include "service_lifecycle_guard.h"

static const char *TAG = LOG_TAG_LED;

esp_err_t led_service_init(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "led init denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = led_output_ws2812_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 backend init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t led_service_start_task(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "led start denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    return led_task_start();
}

esp_err_t led_service_stop_task(void)
{
    if (!service_lifecycle_guard_is_owned()) {
        ESP_LOGE(TAG, "led stop denied: lifecycle is owned by service_runtime");
        return ESP_ERR_INVALID_STATE;
    }
    (void)led_service_stop(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(20));
    return led_task_stop();
}

esp_err_t led_service_play_scene(led_scene_id_t scene_id, uint32_t duration_ms, uint32_t timeout_ms)
{
    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_PLAY_SCENE;
    cmd.payload.play_scene.scene_id = scene_id;
    cmd.payload.play_scene.duration_ms = duration_ms;
    return app_tasking_send_led_command(&cmd, timeout_ms);
}

esp_err_t led_service_stop(uint32_t timeout_ms)
{
    led_command_t cmd = { .id = LED_CMD_STOP };
    return app_tasking_send_led_command(&cmd, timeout_ms);
}

esp_err_t led_service_set_brightness(led_brightness_t brightness, uint32_t timeout_ms)
{
    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_SET_BRIGHTNESS;
    cmd.payload.set_brightness.brightness = brightness;
    return app_tasking_send_led_command(&cmd, timeout_ms);
}
