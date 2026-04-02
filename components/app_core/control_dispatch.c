#include "control_dispatch.h"

#include <inttypes.h>
#include <stdio.h>
#include "app_tasking.h"
#include "esp_check.h"
#include "esp_log.h"
#include "log_tags.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MODE_MANAGER;

esp_err_t control_dispatch_queue_led_scene(uint32_t scene_id, uint32_t duration_ms)
{
    led_command_t led_cmd = { 0 };
    led_cmd.id = LED_CMD_PLAY_SCENE;
    led_cmd.payload.play_scene.scene_id = scene_id;
    led_cmd.payload.play_scene.duration_ms = duration_ms;
    ESP_RETURN_ON_ERROR(app_tasking_send_led_command(&led_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue LED scene");
    ESP_LOGI(TAG, "queued LED scene id=%" PRIu32 " duration=%" PRIu32 "ms", scene_id, duration_ms);
    return ESP_OK;
}

esp_err_t control_dispatch_queue_led_stop(void)
{
    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_STOP;
    return app_tasking_send_led_command(&cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t control_dispatch_queue_led_touch_zone(uint8_t zone_id, bool pressed)
{
    led_command_t led_cmd = { 0 };
    led_cmd.id = LED_CMD_TOUCH_ZONE_SET;
    led_cmd.payload.touch_zone.zone_id = zone_id;
    led_cmd.payload.touch_zone.pressed = pressed ? 1U : 0U;
    return app_tasking_send_led_command(&led_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t control_dispatch_queue_led_touch_overlay_clear(void)
{
    led_command_t led_cmd = { 0 };
    led_cmd.id = LED_CMD_TOUCH_OVERLAY_CLEAR;
    return app_tasking_send_led_command(&led_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t control_dispatch_queue_audio_asset(uint32_t asset_id)
{
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_PLAY_ASSET;
    audio_cmd.payload.play_asset.asset_id = asset_id;
    ESP_RETURN_ON_ERROR(app_tasking_send_audio_command(&audio_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue audio asset");
    ESP_LOGI(TAG, "queued audio asset id=%" PRIu32, asset_id);
    return ESP_OK;
}

esp_err_t control_dispatch_queue_audio_stop(void)
{
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_STOP;
    return app_tasking_send_audio_command(&audio_cmd, CONFIG_ORB_AUDIO_STOP_TIMEOUT_MS);
}

esp_err_t control_dispatch_queue_audio_set_volume(uint8_t volume)
{
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_SET_VOLUME;
    audio_cmd.payload.set_volume.volume = volume;
    ESP_RETURN_ON_ERROR(app_tasking_send_audio_command(&audio_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue audio set volume");
    ESP_LOGI(TAG, "queued audio set volume=%u", (unsigned)volume);
    return ESP_OK;
}

esp_err_t control_dispatch_queue_audio_bg_start(uint32_t fade_in_ms, uint16_t gain_permille)
{
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_BG_START;
    audio_cmd.payload.bg_start.fade_in_ms = fade_in_ms;
    audio_cmd.payload.bg_start.gain_permille = gain_permille;
    ESP_RETURN_ON_ERROR(app_tasking_send_audio_command(&audio_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue audio bg start");
    ESP_LOGI(TAG, "queued audio bg start fade=%" PRIu32 "ms gain=%u", fade_in_ms, (unsigned)gain_permille);
    return ESP_OK;
}

esp_err_t control_dispatch_queue_audio_bg_set_gain(uint32_t fade_ms, uint16_t gain_permille)
{
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_BG_SET_GAIN;
    audio_cmd.payload.bg_set_gain.fade_ms = fade_ms;
    audio_cmd.payload.bg_set_gain.gain_permille = gain_permille;
    ESP_RETURN_ON_ERROR(app_tasking_send_audio_command(&audio_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue audio bg set gain");
    ESP_LOGI(TAG, "queued audio bg set gain fade=%" PRIu32 "ms gain=%u", fade_ms, (unsigned)gain_permille);
    return ESP_OK;
}

esp_err_t control_dispatch_queue_audio_bg_fade_out(uint32_t fade_out_ms)
{
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_BG_FADE_OUT;
    audio_cmd.payload.bg_fade_out.fade_out_ms = fade_out_ms;
    ESP_RETURN_ON_ERROR(app_tasking_send_audio_command(&audio_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue audio bg fade-out");
    ESP_LOGI(TAG, "queued audio bg fade-out=%" PRIu32 "ms", fade_out_ms);
    return ESP_OK;
}

esp_err_t control_dispatch_queue_audio_bg_stop(void)
{
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_BG_STOP;
    return app_tasking_send_audio_command(&audio_cmd, CONFIG_ORB_AUDIO_STOP_TIMEOUT_MS);
}

esp_err_t control_dispatch_queue_audio_pcm_stream_start(void)
{
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_PCM_STREAM_START;
    ESP_RETURN_ON_ERROR(app_tasking_send_audio_command(&audio_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue audio pcm stream start");
    ESP_LOGI(TAG, "queued audio pcm stream start");
    return ESP_OK;
}

esp_err_t control_dispatch_queue_audio_pcm_stream_stop(void)
{
    audio_command_t audio_cmd = { 0 };
    audio_cmd.id = AUDIO_CMD_PCM_STREAM_STOP;
    ESP_RETURN_ON_ERROR(app_tasking_send_audio_command(&audio_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue audio pcm stream stop");
    ESP_LOGI(TAG, "queued audio pcm stream stop");
    return ESP_OK;
}

esp_err_t control_dispatch_queue_led_brightness(uint8_t brightness)
{
    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_SET_BRIGHTNESS;
    cmd.payload.set_brightness.brightness = brightness;
    return app_tasking_send_led_command(&cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t control_dispatch_queue_led_aura_color(uint8_t r, uint8_t g, uint8_t b, uint32_t ramp_ms)
{
    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_SET_AURA_COLOR;
    cmd.payload.aura_color.r = r;
    cmd.payload.aura_color.g = g;
    cmd.payload.aura_color.b = b;
    cmd.payload.aura_color.ramp_ms = ramp_ms;
    return app_tasking_send_led_command(&cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t control_dispatch_queue_led_aura_fade_out(uint32_t duration_ms)
{
    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_AURA_FADE_OUT;
    cmd.payload.aura_fade_out.duration_ms = duration_ms;
    return app_tasking_send_led_command(&cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t control_dispatch_queue_led_audio_level(uint8_t level)
{
    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_SET_AUDIO_REACTIVE_LEVEL;
    cmd.payload.audio_level.level = level;
    return app_tasking_send_led_command(&cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t control_dispatch_queue_mic_start_capture(uint32_t capture_id, uint32_t max_capture_ms)
{
    mic_command_t mic_cmd = { 0 };
    mic_cmd.id = MIC_CMD_START_CAPTURE;
    mic_cmd.payload.start_capture.capture_id = capture_id;
    mic_cmd.payload.start_capture.max_capture_ms = max_capture_ms;
    ESP_RETURN_ON_ERROR(app_tasking_send_mic_command(&mic_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue mic capture start");
    ESP_LOGI(TAG, "queued mic capture start id=%" PRIu32 " duration=%" PRIu32 "ms", capture_id, max_capture_ms);
    return ESP_OK;
}

esp_err_t control_dispatch_queue_mic_stop_capture(void)
{
    mic_command_t mic_cmd = { .id = MIC_CMD_STOP_CAPTURE };
    return app_tasking_send_mic_command(&mic_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t control_dispatch_queue_mic_tts_play_text(const char *text, uint32_t stream_timeout_ms)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    mic_command_t mic_cmd = { 0 };
    mic_cmd.id = MIC_CMD_TTS_PLAY_TEXT;
    (void)snprintf(mic_cmd.payload.tts_play.text, sizeof(mic_cmd.payload.tts_play.text), "%s", text);
    mic_cmd.payload.tts_play.timeout_ms = stream_timeout_ms;
    ESP_RETURN_ON_ERROR(app_tasking_send_mic_command(&mic_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue mic tts play");
    ESP_LOGI(TAG, "queued mic tts play timeout=%" PRIu32 "ms text=%.40s", stream_timeout_ms, text);
    return ESP_OK;
}

esp_err_t control_dispatch_queue_mic_loopback_start(void)
{
    mic_command_t mic_cmd = { .id = MIC_CMD_LOOPBACK_START };
    ESP_RETURN_ON_ERROR(app_tasking_send_mic_command(&mic_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue mic loopback start");
    ESP_LOGI(TAG, "queued mic loopback start");
    return ESP_OK;
}

esp_err_t control_dispatch_queue_mic_loopback_stop(void)
{
    mic_command_t mic_cmd = { .id = MIC_CMD_LOOPBACK_STOP };
    ESP_RETURN_ON_ERROR(app_tasking_send_mic_command(&mic_cmd, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS),
                        TAG,
                        "failed to queue mic loopback stop");
    ESP_LOGI(TAG, "queued mic loopback stop");
    return ESP_OK;
}
