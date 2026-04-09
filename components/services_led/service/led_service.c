#include "led_service.h"

#include "app_tasking.h"
#include "esp_log.h"
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

esp_err_t led_service_clear(uint32_t timeout_ms)
{
    led_command_t cmd = { .id = LED_CMD_CLEAR };
    return app_tasking_send_led_command(&cmd, timeout_ms);
}

esp_err_t led_service_stop(uint32_t timeout_ms)
{
    /* Backward-compatible API name: semantic is content clear, not task lifecycle stop. */
    return led_service_clear(timeout_ms);
}

esp_err_t led_service_set_brightness(led_brightness_t brightness, uint32_t timeout_ms)
{
    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_SET_BRIGHTNESS;
    cmd.payload.set_brightness.brightness = brightness;
    return app_tasking_send_led_command(&cmd, timeout_ms);
}

esp_err_t led_service_set_effect_params(uint8_t speed, uint8_t intensity, uint8_t scale, uint32_t timeout_ms)
{
    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_SET_EFFECT_PARAMS;
    cmd.payload.set_effect_params.speed = speed;
    cmd.payload.set_effect_params.intensity = intensity;
    cmd.payload.set_effect_params.scale = scale;
    return app_tasking_send_led_command(&cmd, timeout_ms);
}

esp_err_t led_service_set_effect_palette(uint8_t mode,
                                         uint8_t color1_r,
                                         uint8_t color1_g,
                                         uint8_t color1_b,
                                         uint8_t color2_r,
                                         uint8_t color2_g,
                                         uint8_t color2_b,
                                         uint8_t color3_r,
                                         uint8_t color3_g,
                                         uint8_t color3_b,
                                         uint32_t timeout_ms)
{
    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_SET_EFFECT_PALETTE;
    cmd.payload.set_effect_palette.mode = mode;
    cmd.payload.set_effect_palette.c1_r = color1_r;
    cmd.payload.set_effect_palette.c1_g = color1_g;
    cmd.payload.set_effect_palette.c1_b = color1_b;
    cmd.payload.set_effect_palette.c2_r = color2_r;
    cmd.payload.set_effect_palette.c2_g = color2_g;
    cmd.payload.set_effect_palette.c2_b = color2_b;
    cmd.payload.set_effect_palette.c3_r = color3_r;
    cmd.payload.set_effect_palette.c3_g = color3_g;
    cmd.payload.set_effect_palette.c3_b = color3_b;
    return app_tasking_send_led_command(&cmd, timeout_ms);
}
