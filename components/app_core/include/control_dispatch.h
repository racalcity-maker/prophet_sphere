#ifndef CONTROL_DISPATCH_H
#define CONTROL_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t control_dispatch_queue_led_scene(uint32_t scene_id, uint32_t duration_ms);
esp_err_t control_dispatch_queue_led_stop(void);
esp_err_t control_dispatch_queue_led_touch_zone(uint8_t zone_id, bool pressed);
esp_err_t control_dispatch_queue_led_touch_overlay_clear(void);
esp_err_t control_dispatch_queue_led_brightness(uint8_t brightness);
esp_err_t control_dispatch_queue_led_aura_color(uint8_t r, uint8_t g, uint8_t b, uint32_t ramp_ms);
esp_err_t control_dispatch_queue_led_aura_fade_out(uint32_t duration_ms);
esp_err_t control_dispatch_queue_led_audio_level(uint8_t level);

esp_err_t control_dispatch_queue_audio_asset(uint32_t asset_id);
esp_err_t control_dispatch_queue_audio_stop(void);
esp_err_t control_dispatch_queue_audio_set_volume(uint8_t volume);
esp_err_t control_dispatch_queue_audio_bg_start(uint32_t fade_in_ms, uint16_t gain_permille);
esp_err_t control_dispatch_queue_audio_bg_set_gain(uint32_t fade_ms, uint16_t gain_permille);
esp_err_t control_dispatch_queue_audio_bg_fade_out(uint32_t fade_out_ms);
esp_err_t control_dispatch_queue_audio_bg_stop(void);
esp_err_t control_dispatch_queue_audio_pcm_stream_start(void);
esp_err_t control_dispatch_queue_audio_pcm_stream_stop(void);

esp_err_t control_dispatch_queue_mic_start_capture(uint32_t capture_id, uint32_t max_capture_ms);
esp_err_t control_dispatch_queue_mic_stop_capture(void);
esp_err_t control_dispatch_queue_mic_tts_play_text(const char *text, uint32_t stream_timeout_ms);
esp_err_t control_dispatch_queue_mic_loopback_start(void);
esp_err_t control_dispatch_queue_mic_loopback_stop(void);

#ifdef __cplusplus
}
#endif

#endif

