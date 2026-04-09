#include "bsp_submode_button.h"

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_SUBMODE_BUTTON;
static TaskHandle_t s_submode_button_task;
static const uint32_t SUBMODE_BUTTON_MIN_STACK = 3072U;
static bsp_submode_button_pressed_cb_t s_pressed_callback;
static void *s_pressed_callback_ctx;

static uint32_t calc_debounce_samples(uint32_t poll_ms, uint32_t debounce_ms)
{
    if (poll_ms == 0U) {
        poll_ms = 1U;
    }
    uint32_t samples = (debounce_ms + poll_ms - 1U) / poll_ms;
    return samples > 0U ? samples : 1U;
}

esp_err_t bsp_submode_button_set_pressed_callback(bsp_submode_button_pressed_cb_t callback, void *user_ctx)
{
    s_pressed_callback = callback;
    s_pressed_callback_ctx = user_ctx;
    return ESP_OK;
}

static void submode_button_task(void *arg)
{
    (void)arg;
    const gpio_num_t pin = (gpio_num_t)CONFIG_ORB_SUBMODE_BUTTON_GPIO;
    const uint32_t poll_ms = (uint32_t)CONFIG_ORB_SUBMODE_BUTTON_POLL_MS;
    const uint32_t debounce_samples = calc_debounce_samples((uint32_t)CONFIG_ORB_SUBMODE_BUTTON_POLL_MS,
                                                            (uint32_t)CONFIG_ORB_SUBMODE_BUTTON_DEBOUNCE_MS);

    bool stable_pressed = false;
    bool candidate_pressed = false;
    uint32_t stable_counter = 0U;
    TickType_t delay_ticks = pdMS_TO_TICKS(poll_ms);
    if (delay_ticks == 0) {
        delay_ticks = 1;
    }

    ESP_LOGI(TAG,
             "submode button task started pin=%d poll=%lums debounce=%lums initial_level=%d active_low=1",
             (int)pin,
             (unsigned long)poll_ms,
             (unsigned long)CONFIG_ORB_SUBMODE_BUTTON_DEBOUNCE_MS,
             gpio_get_level(pin));

    while (true) {
        bool sample_pressed = (gpio_get_level(pin) == 0);

        if (sample_pressed == candidate_pressed) {
            if (stable_counter < UINT32_MAX) {
                stable_counter++;
            }
        } else {
            candidate_pressed = sample_pressed;
            stable_counter = 1U;
        }

        if (candidate_pressed != stable_pressed && stable_counter >= debounce_samples) {
            stable_pressed = candidate_pressed;
            if (stable_pressed) {
                if (s_pressed_callback != NULL) {
                    s_pressed_callback(s_pressed_callback_ctx);
                } else {
                    ESP_LOGW(TAG, "submode button pressed, callback is not set");
                }
            } else {
                ESP_LOGI(TAG, "submode button released");
            }
        }

        vTaskDelay(delay_ticks);
    }
}

esp_err_t bsp_submode_button_start(void)
{
#if !CONFIG_ORB_SUBMODE_BUTTON_ENABLE
    return ESP_OK;
#else
    if (s_submode_button_task != NULL) {
        return ESP_OK;
    }

    const int pin = CONFIG_ORB_SUBMODE_BUTTON_GPIO;
    if (pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = CONFIG_ORB_SUBMODE_BUTTON_PULLUP_ENABLE ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG,
             "submode button configured pin=%d pullup=%d level=%d",
             pin,
             CONFIG_ORB_SUBMODE_BUTTON_PULLUP_ENABLE ? 1 : 0,
             gpio_get_level((gpio_num_t)pin));

    BaseType_t ok = xTaskCreate(submode_button_task,
                                "submode_button_task",
                                (CONFIG_ORB_SUBMODE_BUTTON_TASK_STACK_SIZE >= (int)SUBMODE_BUTTON_MIN_STACK)
                                    ? CONFIG_ORB_SUBMODE_BUTTON_TASK_STACK_SIZE
                                    : (int)SUBMODE_BUTTON_MIN_STACK,
                                NULL,
                                CONFIG_ORB_SUBMODE_BUTTON_TASK_PRIORITY,
                                &s_submode_button_task);
    if (ok != pdPASS) {
        s_submode_button_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
#endif
}
