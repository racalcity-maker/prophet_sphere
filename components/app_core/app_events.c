#include "app_events.h"

const char *app_event_id_to_str(app_event_id_t id)
{
    switch (id) {
    case APP_EVENT_NONE:
        return "APP_EVENT_NONE";
    case APP_EVENT_TOUCH_DOWN:
        return "APP_EVENT_TOUCH_DOWN";
    case APP_EVENT_TOUCH_UP:
        return "APP_EVENT_TOUCH_UP";
    case APP_EVENT_TOUCH_HOLD:
        return "APP_EVENT_TOUCH_HOLD";
    case APP_EVENT_AUDIO_DONE:
        return "APP_EVENT_AUDIO_DONE";
    case APP_EVENT_AUDIO_ERROR:
        return "APP_EVENT_AUDIO_ERROR";
    case APP_EVENT_AUDIO_LEVEL:
        return "APP_EVENT_AUDIO_LEVEL";
    case APP_EVENT_MIC_CAPTURE_DONE:
        return "APP_EVENT_MIC_CAPTURE_DONE";
    case APP_EVENT_MIC_ERROR:
        return "APP_EVENT_MIC_ERROR";
    case APP_EVENT_MIC_REMOTE_PLAN_READY:
        return "APP_EVENT_MIC_REMOTE_PLAN_READY";
    case APP_EVENT_MIC_REMOTE_PLAN_ERROR:
        return "APP_EVENT_MIC_REMOTE_PLAN_ERROR";
    case APP_EVENT_MIC_TTS_STREAM_STARTED:
        return "APP_EVENT_MIC_TTS_STREAM_STARTED";
    case APP_EVENT_MIC_TTS_DONE:
        return "APP_EVENT_MIC_TTS_DONE";
    case APP_EVENT_MIC_TTS_ERROR:
        return "APP_EVENT_MIC_TTS_ERROR";
    case APP_EVENT_NETWORK_UP:
        return "APP_EVENT_NETWORK_UP";
    case APP_EVENT_NETWORK_DOWN:
        return "APP_EVENT_NETWORK_DOWN";
    case APP_EVENT_MQTT_COMMAND_RX:
        return "APP_EVENT_MQTT_COMMAND_RX";
    case APP_EVENT_AI_RESPONSE_READY:
        return "APP_EVENT_AI_RESPONSE_READY";
    case APP_EVENT_AI_ERROR:
        return "APP_EVENT_AI_ERROR";
    case APP_EVENT_MODE_SWITCH_REQUEST:
        return "APP_EVENT_MODE_SWITCH_REQUEST";
    case APP_EVENT_SUBMODE_BUTTON_REQUEST:
        return "APP_EVENT_SUBMODE_BUTTON_REQUEST";
    case APP_EVENT_NETWORK_RECONFIGURE_REQUEST:
        return "APP_EVENT_NETWORK_RECONFIGURE_REQUEST";
    case APP_EVENT_TIMER_EXPIRED:
        return "APP_EVENT_TIMER_EXPIRED";
    default:
        return "APP_EVENT_UNKNOWN";
    }
}
