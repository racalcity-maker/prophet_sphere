#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include <stdint.h>
#include "app_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_EVENT_NONE = 0,
    APP_EVENT_TOUCH_DOWN,
    APP_EVENT_TOUCH_UP,
    APP_EVENT_TOUCH_HOLD,
    APP_EVENT_AUDIO_DONE,
    APP_EVENT_AUDIO_ERROR,
    APP_EVENT_AUDIO_LEVEL,
    APP_EVENT_MIC_CAPTURE_DONE,
    APP_EVENT_MIC_ERROR,
    APP_EVENT_MIC_REMOTE_PLAN_READY,
    APP_EVENT_MIC_REMOTE_PLAN_ERROR,
    APP_EVENT_MIC_TTS_STREAM_STARTED,
    APP_EVENT_MIC_TTS_DONE,
    APP_EVENT_MIC_TTS_ERROR,
    APP_EVENT_NETWORK_UP,
    APP_EVENT_NETWORK_DOWN,
    APP_EVENT_MQTT_COMMAND_RX,
    APP_EVENT_AI_RESPONSE_READY,
    APP_EVENT_AI_ERROR,
    APP_EVENT_MODE_SWITCH_REQUEST,
    APP_EVENT_SUBMODE_BUTTON_REQUEST,
    APP_EVENT_NETWORK_RECONFIGURE_REQUEST,
    APP_EVENT_TIMER_EXPIRED,
} app_event_id_t;

typedef enum {
    APP_EVENT_SOURCE_UNKNOWN = 0,
    APP_EVENT_SOURCE_TOUCH,
    APP_EVENT_SOURCE_AUDIO,
    APP_EVENT_SOURCE_MIC,
    APP_EVENT_SOURCE_NETWORK,
    APP_EVENT_SOURCE_MQTT,
    APP_EVENT_SOURCE_AI,
    APP_EVENT_SOURCE_CONTROL,
    APP_EVENT_SOURCE_TIMER,
} app_event_source_t;

typedef enum {
    APP_TIMER_KIND_NONE = 0,
    APP_TIMER_KIND_SESSION_COOLDOWN = 1,
    APP_TIMER_KIND_AUDIO_SEQUENCE_GAP = 2,
    APP_TIMER_KIND_GRUMBLE_FADE = 3,
    APP_TIMER_KIND_LOTTERY = 4,
    APP_TIMER_KIND_MODE_AUDIO_GAP = 5,
    APP_TIMER_KIND_HYBRID_WS_TIMEOUT = 6,
} app_timer_kind_t;

typedef enum {
    APP_AUDIO_DONE_CODE_NONE = 0,
    APP_AUDIO_DONE_CODE_BG_FADE_COMPLETE = 1,
} app_audio_done_code_t;

typedef union {
    struct {
        uint8_t zone_id;
        uint8_t reserved;
        uint16_t strength;
        uint32_t raw;
        uint32_t baseline;
        uint32_t delta;
    } touch;
    struct {
        orb_mode_t from_mode;
        orb_mode_t to_mode;
    } mode;
    struct {
        uint32_t value;
        int32_t code;
    } scalar;
    struct {
        uint32_t capture_id;
        uint16_t level_avg;
        uint16_t level_peak;
        uint16_t intent_confidence_permille;
        uint8_t intent_id;
        uint8_t reserved;
    } mic;
    struct {
        char text[32];
    } text;
} app_event_payload_t;

typedef struct app_event {
    app_event_id_t id;
    app_event_source_t source;
    uint32_t timestamp_ms;
    app_event_payload_t payload;
} app_event_t;

const char *app_event_id_to_str(app_event_id_t id);

#ifdef __cplusplus
}
#endif

#endif
