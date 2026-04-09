#include "led_task_internal.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_effects.h"
#include "led_output_ws2812.h"
#include "led_power_limit.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_LED;

void led_task_wake(void)
{
    if (s_led_task_handle == NULL) {
        return;
    }
    /* led_task sleeps in vTaskDelayUntil(); abort-delay is the reliable wake path. */
    xTaskAbortDelay(s_led_task_handle);
}

void led_task_entry(void *arg)
{
    (void)arg;

    QueueHandle_t queue = app_tasking_get_led_cmd_queue();
    if (queue == NULL) {
        ESP_LOGE(TAG, "led_cmd_queue is not initialized");
        s_led_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint32_t now_ms = led_task_tick_to_ms(xTaskGetTickCount());
    led_task_init_runtime_defaults(&s_runtime, now_ms);

    ESP_LOGI(TAG,
             "led_task started matrix=%ux%u fps=%u palette=%u",
             (unsigned)LED_MATRIX_W,
             (unsigned)LED_MATRIX_H,
             (unsigned)(1000U / CONFIG_ORB_LED_FRAME_INTERVAL_MS),
             (unsigned)s_runtime.effect_palette_mode);
    ESP_LOGI(TAG,
             "led defaults brightness=%u limiter=%s cap=%umA channel=%umA idle=%umA",
             (unsigned)s_runtime.brightness,
#if CONFIG_ORB_LED_POWER_LIMIT_ENABLE
             "on",
             (unsigned)CONFIG_ORB_LED_MAX_CURRENT_MA,
             (unsigned)CONFIG_ORB_LED_MAX_COLOR_CHANNEL_MA,
             (unsigned)CONFIG_ORB_LED_IDLE_CURRENT_MA);
#else
             "off",
             0U,
             0U,
             0U);
#endif

    TickType_t last_wake = xTaskGetTickCount();
    TickType_t frame_period = led_task_frame_ticks();
    while (!s_stop_requested) {
        led_command_t cmd = { 0 };
        while (xQueueReceive(queue, &cmd, 0) == pdTRUE) {
            led_task_handle_command(&s_runtime, &cmd);
        }
        if (s_stop_requested) {
            break;
        }

        now_ms = led_task_tick_to_ms(xTaskGetTickCount());
        led_task_maybe_apply_scene_timeout(&s_runtime, now_ms);
        led_task_maybe_update_aura_transition(&s_runtime, now_ms);
        uint32_t scene_elapsed_ms = now_ms - s_runtime.scene_started_ms;
        led_effects_render_scene(s_runtime.scene_id,
                                 &s_runtime.effects,
                                 now_ms,
                                 scene_elapsed_ms,
                                 s_runtime.effect_speed,
                                 s_runtime.effect_intensity,
                                 s_runtime.effect_scale,
                                 s_runtime.brightness,
                                 led_task_effects_set_pixel_cb,
                                 led_task_effects_fill_cb,
                                 led_task_effects_clear_cb,
                                 NULL);
        led_task_apply_effect_palette(&s_runtime);
        led_task_apply_touch_overlay(&s_runtime, now_ms);
        led_task_apply_audio_reactive_gain(&s_runtime, now_ms);
        led_task_apply_scene_transition_blend(&s_runtime, now_ms);

        if (s_runtime.scene_id != 0U) {
            led_power_limit_result_t limit_result = { 0 };
            led_power_limit_apply_grb(s_framebuffer, sizeof(s_framebuffer), &limit_result);
            if (limit_result.limited && (now_ms - s_last_limit_log_ms) >= 2000U) {
                s_last_limit_log_ms = now_ms;
                ESP_LOGI(TAG,
                         "power limit active est=%" PRIu32 "mA cap=%umA scale=%u.%u%%",
                         limit_result.estimated_current_ma_before,
                         (unsigned)CONFIG_ORB_LED_MAX_CURRENT_MA,
                         (unsigned)(limit_result.applied_scale_permille / 10U),
                         (unsigned)(limit_result.applied_scale_permille % 10U));
            }

            esp_err_t err = led_output_ws2812_write_grb(s_framebuffer, sizeof(s_framebuffer), CONFIG_ORB_LED_TX_TIMEOUT_MS);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "frame TX failed: %s", esp_err_to_name(err));
            }
        }

        vTaskDelayUntil(&last_wake, frame_period);
    }

    memset(&s_runtime, 0, sizeof(s_runtime));
    led_task_framebuffer_clear();
    (void)led_output_ws2812_clear(CONFIG_ORB_LED_TX_TIMEOUT_MS);
    s_led_task_handle = NULL;
    s_stop_requested = false;
    ESP_LOGI(TAG, "led_task stopped");
    vTaskDelete(NULL);
}

