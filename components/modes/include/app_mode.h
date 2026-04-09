#ifndef APP_MODE_H
#define APP_MODE_H

#include <stdint.h>
#include "app_defs.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MODE_EVENT_NONE = 0,
    APP_MODE_EVENT_TOUCH_DOWN,
    APP_MODE_EVENT_TOUCH_UP,
    APP_MODE_EVENT_TOUCH_HOLD,
    APP_MODE_EVENT_AUDIO_DONE,
    APP_MODE_EVENT_AUDIO_ERROR,
    APP_MODE_EVENT_MIC_CAPTURE_DONE,
    APP_MODE_EVENT_MIC_ERROR,
    APP_MODE_EVENT_MIC_REMOTE_PLAN_READY,
    APP_MODE_EVENT_MIC_REMOTE_PLAN_ERROR,
    APP_MODE_EVENT_MIC_TTS_STREAM_STARTED,
    APP_MODE_EVENT_MIC_TTS_DONE,
    APP_MODE_EVENT_MIC_TTS_ERROR,
    APP_MODE_EVENT_AI_RESPONSE_READY,
    APP_MODE_EVENT_AI_ERROR,
    APP_MODE_EVENT_TIMER_EXPIRED,
    APP_MODE_EVENT_NETWORK_UP,
    APP_MODE_EVENT_NETWORK_DOWN,
    APP_MODE_EVENT_MQTT_COMMAND_RX,
    APP_MODE_EVENT_UNKNOWN,
} app_mode_event_id_t;

#define APP_MODE_EVENT_TEXT_MAX_LEN 32
#define APP_MODE_ACTION_TTS_TEXT_MAX_LEN 160

typedef struct {
    app_mode_event_id_t id;
    uint32_t value; /* Numeric payload for touch zone/scalar/timer events. */
    int32_t code;   /* Extended payload code for AUDIO_DONE/ERROR and timers. */
    uint32_t aux;   /* Secondary numeric payload (MIC level_avg, etc). */
    uint16_t confidence_permille; /* Optional classifier confidence for MIC events. */
    uint8_t intent_id;            /* Optional classifier label for MIC events. */
    uint8_t reserved;
    /* Text payload for APP_MODE_EVENT_MQTT_COMMAND_RX, zero-terminated. */
    char text[APP_MODE_EVENT_TEXT_MAX_LEN];
} app_mode_event_t;

typedef enum {
    APP_MODE_AUDIO_DONE_CODE_NONE = 0,
    APP_MODE_AUDIO_DONE_CODE_BG_FADE_COMPLETE = 1,
} app_mode_audio_done_code_t;

typedef enum {
    APP_MODE_ACTION_NONE = 0,
    APP_MODE_ACTION_START_INTERACTION,
    APP_MODE_ACTION_PLAY_AUDIO_ASSET,
    APP_MODE_ACTION_PLAY_GRUMBLE,
    APP_MODE_ACTION_START_AUDIO_SEQUENCE,
    APP_MODE_ACTION_BEGIN_COOLDOWN,
    APP_MODE_ACTION_AURA_FADE_OUT,
    APP_MODE_ACTION_RETURN_IDLE,
    APP_MODE_ACTION_LED_SET_SCENE,
    APP_MODE_ACTION_LOTTERY_START_SORTING,
    APP_MODE_ACTION_LOTTERY_ASSIGN_TEAM,
    APP_MODE_ACTION_LOTTERY_ABORT,
    APP_MODE_ACTION_LOTTERY_RETURN_IDLE,
    APP_MODE_ACTION_PROPHECY_START,
    APP_MODE_ACTION_AUDIO_BG_START,
    APP_MODE_ACTION_AUDIO_BG_SET_GAIN,
    APP_MODE_ACTION_AUDIO_BG_FADE_OUT,
    APP_MODE_ACTION_AUDIO_BG_STOP,
    APP_MODE_ACTION_HYBRID_WS_TIMER_START,
    APP_MODE_ACTION_MIC_START_CAPTURE,
    APP_MODE_ACTION_MIC_STOP_CAPTURE,
    APP_MODE_ACTION_MIC_TTS_PLAY_TEXT,
    APP_MODE_ACTION_MIC_LOOPBACK_START,
    APP_MODE_ACTION_MIC_LOOPBACK_STOP,
} app_mode_action_id_t;

typedef enum {
    APP_MODE_SEQUENCE_GENERIC = 0,
    APP_MODE_SEQUENCE_AURA = 1,
    APP_MODE_SEQUENCE_PROPHECY = 2,
} app_mode_sequence_kind_t;

typedef enum {
    APP_MODE_LOTTERY_AUDIO_KIND_ASSET = 0,
    APP_MODE_LOTTERY_AUDIO_KIND_TRACK_PATH = 1,
    APP_MODE_LOTTERY_AUDIO_KIND_TTS = 2,
} app_mode_lottery_audio_kind_t;

typedef struct {
    uint32_t scene_id;
    uint32_t duration_ms;
    uint32_t fade_ms;
    uint8_t color_r;
    uint8_t color_g;
    uint8_t color_b;
    uint8_t reserved;
} app_mode_action_led_payload_t;

typedef struct {
    uint32_t asset_id;
    uint32_t asset_id_2;
    uint32_t gap_ms;
    app_mode_sequence_kind_t sequence_kind;
} app_mode_action_audio_payload_t;

typedef struct {
    uint32_t fade_ms;
    uint16_t gain_permille;
    uint16_t fg_gain_permille;
} app_mode_action_bg_payload_t;

typedef struct {
    uint32_t capture_id;
    uint32_t capture_ms;
    uint32_t ws_timeout_ms;
    uint32_t tts_timeout_ms;
    char tts_text[APP_MODE_ACTION_TTS_TEXT_MAX_LEN];
} app_mode_action_mic_payload_t;

typedef struct {
    uint32_t cooldown_ms;
} app_mode_action_timing_payload_t;

typedef struct {
    app_mode_action_id_t id;
    app_mode_action_led_payload_t led;
    app_mode_action_audio_payload_t audio;
    app_mode_action_bg_payload_t bg;
    app_mode_action_mic_payload_t mic;
    app_mode_action_timing_payload_t timing;
} app_mode_action_t;

typedef struct app_mode {
    orb_mode_t id;
    const char *name;
    esp_err_t (*init)(void);
    esp_err_t (*enter)(void);
    esp_err_t (*exit)(void);
    esp_err_t (*handle_event)(const app_mode_event_t *event, app_mode_action_t *action);
} app_mode_t;

const app_mode_t *mode_offline_scripted_get(void);
const app_mode_t *mode_hybrid_ai_get(void);
const app_mode_t *mode_installation_get(void);

#ifdef __cplusplus
}
#endif

#endif
