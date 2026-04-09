#include "mode_switch_cleanup.h"

#include <stdint.h>
#include "config_manager.h"
#include "control_dispatch.h"
#include "esp_log.h"
#include "interaction_sequence.h"
#include "log_tags.h"
#include "sdkconfig.h"
#include "session_controller.h"

static const char *TAG = LOG_TAG_MODE_MANAGER;

static void queue_audio_stop_quick(void)
{
    esp_err_t err = control_dispatch_queue_audio_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "quick queue audio stop failed: %s", esp_err_to_name(err));
    }
}

static void queue_audio_pcm_stop_quick(void)
{
    esp_err_t err = control_dispatch_queue_audio_pcm_stream_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "quick queue audio pcm stop failed: %s", esp_err_to_name(err));
    }
}

static void queue_mic_stop_capture_quick(void)
{
    esp_err_t err = control_dispatch_queue_mic_stop_capture();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "quick queue mic stop capture failed: %s", esp_err_to_name(err));
    }
}

static void queue_mic_loopback_stop_quick(void)
{
    esp_err_t err = control_dispatch_queue_mic_loopback_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "quick queue mic loopback stop failed: %s", esp_err_to_name(err));
    }
}

esp_err_t mode_switch_cleanup_run(mode_action_executor_t *executor, mode_timers_t *timers)
{
    if (executor == NULL || timers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    (void)control_dispatch_queue_led_stop();
    queue_audio_stop_quick();
    queue_audio_pcm_stop_quick();
    queue_mic_stop_capture_quick();
    queue_mic_loopback_stop_quick();
    (void)mode_timers_stop_all(timers);

    (void)session_controller_reset_to_idle();
    (void)interaction_sequence_reset();
    mode_action_executor_reset(executor);

    uint8_t brightness = CONFIG_ORB_LED_DEFAULT_BRIGHTNESS;
    if (config_manager_get_led_brightness(&brightness) != ESP_OK) {
        brightness = CONFIG_ORB_LED_DEFAULT_BRIGHTNESS;
    }
    (void)control_dispatch_queue_led_brightness(brightness);
    return ESP_OK;
}
