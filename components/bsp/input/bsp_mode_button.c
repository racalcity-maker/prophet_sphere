#include "bsp_mode_button.h"

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "mode_manager.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MODE_BUTTON;
static TaskHandle_t s_mode_button_task;
static const uint32_t MODE_BUTTON_MIN_STACK = 3072U;

static orb_mode_t next_mode(orb_mode_t current)
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

static uint32_t calc_debounce_samples(uint32_t poll_ms, uint32_t debounce_ms)
{
    if (poll_ms == 0U) {
        poll_ms = 1U;
    }
    uint32_t samples = (debounce_ms + poll_ms - 1U) / poll_ms;
    return samples > 0U ? samples : 1U;
}

static void request_next_mode(void)
{
    orb_mode_t current = mode_manager_get_current_mode();
    orb_mode_t target = next_mode(current);
    esp_err_t err = mode_manager_request_switch(target);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mode button request failed: %d -> %d (%s)", (int)current, (int)target, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "mode button: %d -> %d", (int)current, (int)target);
}

static void mode_button_task(void *arg)
{
    (void)arg;
    const gpio_num_t pin = (gpio_num_t)CONFIG_ORB_MODE_BUTTON_GPIO;
    const uint32_t poll_ms = (uint32_t)CONFIG_ORB_MODE_BUTTON_POLL_MS;
    const uint32_t debounce_samples =
        calc_debounce_samples((uint32_t)CONFIG_ORB_MODE_BUTTON_POLL_MS, (uint32_t)CONFIG_ORB_MODE_BUTTON_DEBOUNCE_MS);

    bool stable_pressed = false;
    bool candidate_pressed = false;
    uint32_t stable_counter = 0U;
    TickType_t delay_ticks = pdMS_TO_TICKS(poll_ms);
    if (delay_ticks == 0) {
        delay_ticks = 1;
    }

    ESP_LOGI(TAG,
             "mode button task started pin=%d poll=%lums debounce=%lums initial_level=%d active_low=1",
             (int)pin,
             (unsigned long)poll_ms,
             (unsigned long)CONFIG_ORB_MODE_BUTTON_DEBOUNCE_MS,
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
                ESP_LOGI(TAG, "mode button pressed");
                request_next_mode();
            } else {
                ESP_LOGI(TAG, "mode button released");
            }
        }

        vTaskDelay(delay_ticks);
    }
}

esp_err_t bsp_mode_button_start(void)
{
#if !CONFIG_ORB_MODE_BUTTON_ENABLE
    return ESP_OK;
#else
    if (s_mode_button_task != NULL) {
        return ESP_OK;
    }

    const int pin = CONFIG_ORB_MODE_BUTTON_GPIO;
    if (pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = CONFIG_ORB_MODE_BUTTON_PULLUP_ENABLE ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG,
             "mode button configured pin=%d pullup=%d level=%d",
             pin,
             CONFIG_ORB_MODE_BUTTON_PULLUP_ENABLE ? 1 : 0,
             gpio_get_level((gpio_num_t)pin));

    BaseType_t ok = xTaskCreate(mode_button_task,
                                "mode_button_task",
                                (CONFIG_ORB_MODE_BUTTON_TASK_STACK_SIZE >= (int)MODE_BUTTON_MIN_STACK)
                                    ? CONFIG_ORB_MODE_BUTTON_TASK_STACK_SIZE
                                    : (int)MODE_BUTTON_MIN_STACK,
                                NULL,
                                CONFIG_ORB_MODE_BUTTON_TASK_PRIORITY,
                                &s_mode_button_task);
    if (ok != pdPASS) {
        s_mode_button_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
#endif
}
