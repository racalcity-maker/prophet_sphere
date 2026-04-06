#include "audio_dac_backend.h"

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp_pins.h"
#include "log_tags.h"

#if CONFIG_ORB_AUDIO_DAC_MAX98357A
static const char *TAG = LOG_TAG_AUDIO_I2S;

static int amp_enable_level(bool on)
{
    bool active_high = CONFIG_ORB_AUDIO_AMP_ENABLE_ACTIVE_HIGH;
    return on ? (active_high ? 1 : 0) : (active_high ? 0 : 1);
}

static esp_err_t max98357a_init(void)
{
    ESP_LOGI(TAG, "DAC profile: MAX98357A");
#if CONFIG_ORB_AUDIO_USE_AMP_ENABLE_GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BSP_AMP_ENABLE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "amp_en gpio config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_AMP_ENABLE_GPIO, amp_enable_level(false)), TAG, "amp_en set failed");
#endif
    return ESP_OK;
}

static esp_err_t max98357a_start(void)
{
#if CONFIG_ORB_AUDIO_USE_AMP_ENABLE_GPIO
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_AMP_ENABLE_GPIO, amp_enable_level(true)), TAG, "amp_en set failed");
#endif
    return ESP_OK;
}

static esp_err_t max98357a_stop(void)
{
#if CONFIG_ORB_AUDIO_USE_AMP_ENABLE_GPIO
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_AMP_ENABLE_GPIO, amp_enable_level(false)), TAG, "amp_en set failed");
#endif
    return ESP_OK;
}

const audio_dac_backend_t g_audio_dac_backend = {
    .name = "max98357a",
    .init = max98357a_init,
    .start = max98357a_start,
    .stop = max98357a_stop,
};
#endif
