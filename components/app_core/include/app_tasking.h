#ifndef APP_TASKING_H
#define APP_TASKING_H

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ownership and threading model:
 * - app_control_task is the only owner of global orchestration and mode transitions.
 * - touch_task is the only producer of normalized touch events.
 * - led_task is the only owner of LED render state.
 * - audio_task is the only owner of audio playback state.
 * - mic_task is the only owner of microphone capture state.
 * - ai_task is the only owner of AI execution state.
 * Public APIs enqueue commands/events only; they do not do low-level work.
 */

typedef enum {
    LED_CMD_NONE = 0,
    LED_CMD_PLAY_SCENE,
    LED_CMD_STOP,
    LED_CMD_SET_BRIGHTNESS,
    LED_CMD_TOUCH_ZONE_SET,
    LED_CMD_TOUCH_OVERLAY_CLEAR,
    LED_CMD_SET_AURA_COLOR,
    LED_CMD_AURA_FADE_OUT,
    LED_CMD_SET_AUDIO_REACTIVE_LEVEL,
} led_command_id_t;

typedef struct {
    led_command_id_t id;
    union {
        struct {
            uint32_t scene_id;
            uint32_t duration_ms;
        } play_scene;
        struct {
            uint8_t brightness;
        } set_brightness;
        struct {
            uint8_t zone_id;
            uint8_t pressed;
            uint16_t reserved;
        } touch_zone;
        struct {
            uint8_t r;
            uint8_t g;
            uint8_t b;
            uint8_t reserved;
            uint32_t ramp_ms;
        } aura_color;
        struct {
            uint32_t duration_ms;
        } aura_fade_out;
        struct {
            uint8_t level;
        } audio_level;
    } payload;
} led_command_t;

typedef enum {
    AUDIO_CMD_NONE = 0,
    AUDIO_CMD_PLAY_ASSET,
    AUDIO_CMD_STOP,
    AUDIO_CMD_SET_VOLUME,
    AUDIO_CMD_BG_START,
    AUDIO_CMD_BG_SET_GAIN,
    AUDIO_CMD_BG_FADE_OUT,
    AUDIO_CMD_BG_STOP,
    AUDIO_CMD_PCM_STREAM_START,
    AUDIO_CMD_PCM_STREAM_STOP,
    AUDIO_CMD_PCM_STREAM_CHUNK,
} audio_command_id_t;

#define AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES 1024U

typedef struct {
    audio_command_id_t id;
    union {
        struct {
            uint32_t asset_id;
        } play_asset;
        struct {
            uint8_t volume;
        } set_volume;
        struct {
            uint32_t fade_in_ms;
            uint16_t gain_permille;
            uint16_t reserved;
        } bg_start;
        struct {
            uint32_t fade_ms;
            uint16_t gain_permille;
            uint16_t reserved;
        } bg_set_gain;
        struct {
            uint32_t fade_out_ms;
        } bg_fade_out;
        struct {
            uint16_t sample_count;
            int16_t samples[AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES];
        } pcm_stream_chunk;
    } payload;
} audio_command_t;

typedef enum {
    AI_CMD_NONE = 0,
    AI_CMD_REQUEST,
    AI_CMD_CANCEL,
} ai_command_id_t;

typedef struct {
    ai_command_id_t id;
    union {
        struct {
            uint32_t request_id;
            char prompt[64];
        } request;
    } payload;
} ai_command_t;

typedef enum {
    MIC_CMD_NONE = 0,
    MIC_CMD_START_CAPTURE,
    MIC_CMD_STOP_CAPTURE,
    MIC_CMD_LOOPBACK_START,
    MIC_CMD_LOOPBACK_STOP,
    MIC_CMD_TTS_PLAY_TEXT,
} mic_command_id_t;

#define MIC_TTS_TEXT_MAX_LEN 384U

typedef struct {
    mic_command_id_t id;
    union {
        struct {
            uint32_t capture_id;
            uint32_t max_capture_ms;
        } start_capture;
        struct {
            char text[MIC_TTS_TEXT_MAX_LEN];
            uint32_t timeout_ms;
        } tts_play;
    } payload;
} mic_command_t;

esp_err_t app_tasking_init(void);
esp_err_t app_tasking_start_app_control_task(void);

esp_err_t app_tasking_post_event(const app_event_t *event, uint32_t timeout_ms);
/*
 * Reliable timer-event post API:
 * tries queue first, and if queue is busy stores event in an internal pending
 * set for later delivery by app_control_task (coalesced by timer kind).
 */
esp_err_t app_tasking_post_timer_event_reliable(app_timer_kind_t timer_kind, uint32_t timeout_ms);
bool app_tasking_take_pending_timer_event(app_event_t *event);
esp_err_t app_tasking_send_led_command(const led_command_t *command, uint32_t timeout_ms);
esp_err_t app_tasking_send_audio_command(const audio_command_t *command, uint32_t timeout_ms);
esp_err_t app_tasking_send_ai_command(const ai_command_t *command, uint32_t timeout_ms);
esp_err_t app_tasking_send_mic_command(const mic_command_t *command, uint32_t timeout_ms);

QueueHandle_t app_tasking_get_app_event_queue(void);
QueueHandle_t app_tasking_get_led_cmd_queue(void);
QueueHandle_t app_tasking_get_audio_cmd_queue(void);
QueueHandle_t app_tasking_get_ai_cmd_queue(void);
QueueHandle_t app_tasking_get_mic_cmd_queue(void);

#ifdef __cplusplus
}
#endif

#endif
