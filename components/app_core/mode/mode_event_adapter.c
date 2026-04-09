#include "mode_event_adapter.h"

#include <string.h>

app_mode_event_id_t mode_event_adapter_map_id(app_event_id_t id)
{
    switch (id) {
    case APP_EVENT_TOUCH_DOWN:
        return APP_MODE_EVENT_TOUCH_DOWN;
    case APP_EVENT_TOUCH_UP:
        return APP_MODE_EVENT_TOUCH_UP;
    case APP_EVENT_TOUCH_HOLD:
        return APP_MODE_EVENT_TOUCH_HOLD;
    case APP_EVENT_AUDIO_DONE:
        return APP_MODE_EVENT_AUDIO_DONE;
    case APP_EVENT_AUDIO_ERROR:
        return APP_MODE_EVENT_AUDIO_ERROR;
    case APP_EVENT_MIC_CAPTURE_DONE:
        return APP_MODE_EVENT_MIC_CAPTURE_DONE;
    case APP_EVENT_MIC_ERROR:
        return APP_MODE_EVENT_MIC_ERROR;
    case APP_EVENT_MIC_REMOTE_PLAN_READY:
        return APP_MODE_EVENT_MIC_REMOTE_PLAN_READY;
    case APP_EVENT_MIC_REMOTE_PLAN_ERROR:
        return APP_MODE_EVENT_MIC_REMOTE_PLAN_ERROR;
    case APP_EVENT_MIC_TTS_STREAM_STARTED:
        return APP_MODE_EVENT_MIC_TTS_STREAM_STARTED;
    case APP_EVENT_MIC_TTS_DONE:
        return APP_MODE_EVENT_MIC_TTS_DONE;
    case APP_EVENT_MIC_TTS_ERROR:
        return APP_MODE_EVENT_MIC_TTS_ERROR;
    case APP_EVENT_AI_RESPONSE_READY:
        return APP_MODE_EVENT_AI_RESPONSE_READY;
    case APP_EVENT_AI_ERROR:
        return APP_MODE_EVENT_AI_ERROR;
    case APP_EVENT_TIMER_EXPIRED:
        return APP_MODE_EVENT_TIMER_EXPIRED;
    case APP_EVENT_NETWORK_UP:
        return APP_MODE_EVENT_NETWORK_UP;
    case APP_EVENT_NETWORK_DOWN:
        return APP_MODE_EVENT_NETWORK_DOWN;
    case APP_EVENT_MQTT_COMMAND_RX:
        return APP_MODE_EVENT_MQTT_COMMAND_RX;
    default:
        return APP_MODE_EVENT_UNKNOWN;
    }
}

void mode_event_adapter_from_app_event(const app_event_t *event, app_mode_event_t *mode_event)
{
    if (event == NULL || mode_event == NULL) {
        return;
    }

    *mode_event = (app_mode_event_t){ 0 };
    mode_event->id = mode_event_adapter_map_id(event->id);
    mode_event->code = 0;
    mode_event->aux = 0U;
    mode_event->confidence_permille = 0U;
    mode_event->intent_id = 0U;

    if (event->id == APP_EVENT_TOUCH_DOWN || event->id == APP_EVENT_TOUCH_UP || event->id == APP_EVENT_TOUCH_HOLD) {
        mode_event->value = event->payload.touch.zone_id;
    } else if (event->id == APP_EVENT_MQTT_COMMAND_RX) {
        (void)strncpy(mode_event->text, event->payload.text.text, sizeof(mode_event->text) - 1U);
        mode_event->text[sizeof(mode_event->text) - 1U] = '\0';
    } else if (event->id == APP_EVENT_TIMER_EXPIRED) {
        mode_event->code = event->payload.scalar.code;
        mode_event->value = (uint32_t)event->payload.scalar.code;
    } else if (event->id == APP_EVENT_AUDIO_DONE || event->id == APP_EVENT_AUDIO_ERROR) {
        mode_event->code = event->payload.scalar.code;
        mode_event->value = event->payload.scalar.value;
    } else if (event->id == APP_EVENT_MIC_CAPTURE_DONE ||
               event->id == APP_EVENT_MIC_REMOTE_PLAN_READY ||
               event->id == APP_EVENT_MIC_TTS_STREAM_STARTED) {
        mode_event->code = (int32_t)event->payload.mic.level_peak;
        mode_event->value = event->payload.mic.capture_id;
        mode_event->aux = (uint32_t)event->payload.mic.level_avg;
        mode_event->intent_id = event->payload.mic.intent_id;
        mode_event->confidence_permille = event->payload.mic.intent_confidence_permille;
    } else if (event->id == APP_EVENT_MIC_ERROR || event->id == APP_EVENT_MIC_REMOTE_PLAN_ERROR) {
        mode_event->code = event->payload.scalar.code;
        mode_event->value = event->payload.scalar.value;
    } else if (event->id == APP_EVENT_MIC_TTS_DONE || event->id == APP_EVENT_MIC_TTS_ERROR) {
        mode_event->code = event->payload.scalar.code;
        mode_event->value = event->payload.scalar.value;
    } else {
        mode_event->value = event->payload.scalar.value;
    }
}

