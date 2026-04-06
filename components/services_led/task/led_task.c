#include "led_task.h"

#include <inttypes.h>
#include <string.h>
#include "app_tasking.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_effects.h"
#include "led_output_ws2812.h"
#include "led_power_limit.h"
#include "led_scene.h"
#include "led_task_internal.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_LED;
static TaskHandle_t s_led_task_handle;
static volatile bool s_stop_requested;
uint32_t s_last_limit_log_ms;

#ifndef CONFIG_ORB_LED_STOP_TIMEOUT_MS
#define CONFIG_ORB_LED_STOP_TIMEOUT_MS 300
#endif
#ifndef CONFIG_ORB_LED_STOP_SEND_RETRY_COUNT
#define CONFIG_ORB_LED_STOP_SEND_RETRY_COUNT 8
#endif
#ifndef CONFIG_ORB_LED_STOP_SEND_RETRY_DELAY_MS
#define CONFIG_ORB_LED_STOP_SEND_RETRY_DELAY_MS 10
#endif
#ifndef CONFIG_ORB_LED_STOP_SEND_TIMEOUT_MS
#define CONFIG_ORB_LED_STOP_SEND_TIMEOUT_MS 10
#endif

#if (CONFIG_ORB_LED_MATRIX_ROTATE_90_CW || CONFIG_ORB_LED_MATRIX_ROTATE_90_CCW) && \
    (CONFIG_ORB_LED_MATRIX_WIDTH != CONFIG_ORB_LED_MATRIX_HEIGHT)
#error "90-degree LED matrix rotation currently requires square matrix geometry"
#endif

led_runtime_t s_runtime;
uint8_t s_framebuffer[LED_FRAMEBUFFER_BYTES];

static void led_task_wake(void)
{
    if (s_led_task_handle == NULL) {
        return;
    }
#if defined(configUSE_TASK_NOTIFICATIONS) && (configUSE_TASK_NOTIFICATIONS == 1)
    xTaskNotifyGive(s_led_task_handle);
#else
    xTaskAbortDelay(s_led_task_handle);
#endif
}

static void led_task_entry(void *arg)
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

esp_err_t led_task_start(void)
{
    if (s_led_task_handle != NULL) {
        return ESP_OK;
    }
    s_stop_requested = false;

    BaseType_t ok = xTaskCreate(led_task_entry,
                                "led_task",
                                CONFIG_ORB_LED_TASK_STACK_SIZE,
                                NULL,
                                CONFIG_ORB_LED_TASK_PRIORITY,
                                &s_led_task_handle);
    if (ok != pdPASS) {
        s_led_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "task created prio=%d stack=%d", CONFIG_ORB_LED_TASK_PRIORITY, CONFIG_ORB_LED_TASK_STACK_SIZE);
    return ESP_OK;
}

esp_err_t led_task_stop(void)
{
    if (s_led_task_handle == NULL) {
        return ESP_OK;
    }

    s_stop_requested = true;
    QueueHandle_t queue = app_tasking_get_led_cmd_queue();
    bool stop_enqueued = false;
    if (queue != NULL) {
        led_command_t cmd = { .id = LED_CMD_STOP };
        const TickType_t send_timeout_ticks = pdMS_TO_TICKS(CONFIG_ORB_LED_STOP_SEND_TIMEOUT_MS);
        for (uint32_t i = 0; i < (uint32_t)CONFIG_ORB_LED_STOP_SEND_RETRY_COUNT; ++i) {
            if (xQueueSend(queue, &cmd, send_timeout_ticks) == pdTRUE) {
                stop_enqueued = true;
                break;
            }
            led_task_wake();
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ORB_LED_STOP_SEND_RETRY_DELAY_MS));
        }
    }
    if (!stop_enqueued) {
        ESP_LOGW(TAG, "led_task stop command was not enqueued");
    }
    led_task_wake();

    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)CONFIG_ORB_LED_STOP_TIMEOUT_MS + 200U);
    while (s_led_task_handle != NULL) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGW(TAG, "led_task graceful stop timeout");
            TaskHandle_t handle = s_led_task_handle;
            s_led_task_handle = NULL;
            vTaskDelete(handle);
            s_stop_requested = false;
            (void)led_output_ws2812_clear(CONFIG_ORB_LED_TX_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        led_task_wake();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}
