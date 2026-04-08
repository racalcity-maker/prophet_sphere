#include "mode_hybrid_ai_internal.h"

const char *hybrid_flow_name(hybrid_flow_t flow)
{
    switch (flow) {
    case HYBRID_FLOW_IDLE:
        return "idle";
    case HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE:
        return "wait_remote_intro_tts_done";
    case HYBRID_FLOW_WAIT_MIC_DONE:
        return "wait_mic_done";
    case HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE:
        return "wait_remote_retry_tts_done";
    case HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE:
        return "wait_remote_answer_tts_done";
    case HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE:
        return "wait_fail_prompt_done";
    case HYBRID_FLOW_WAIT_BG_FADE_OUT:
        return "wait_bg_fade";
    default:
        return "unknown";
    }
}

const char *hybrid_event_name(app_mode_event_id_t id)
{
    switch (id) {
    case APP_MODE_EVENT_TOUCH_HOLD:
        return "touch_hold";
    case APP_MODE_EVENT_TOUCH_DOWN:
        return "touch_down";
    case APP_MODE_EVENT_TOUCH_UP:
        return "touch_up";
    case APP_MODE_EVENT_AUDIO_DONE:
        return "audio_done";
    case APP_MODE_EVENT_AUDIO_ERROR:
        return "audio_error";
    case APP_MODE_EVENT_MIC_CAPTURE_DONE:
        return "mic_done";
    case APP_MODE_EVENT_MIC_ERROR:
        return "mic_error";
    case APP_MODE_EVENT_MIC_REMOTE_PLAN_READY:
        return "mic_plan_ready";
    case APP_MODE_EVENT_MIC_REMOTE_PLAN_ERROR:
        return "mic_plan_error";
    case APP_MODE_EVENT_MIC_TTS_STREAM_STARTED:
        return "mic_tts_started";
    case APP_MODE_EVENT_MIC_TTS_DONE:
        return "mic_tts_done";
    case APP_MODE_EVENT_MIC_TTS_ERROR:
        return "mic_tts_error";
    case APP_MODE_EVENT_TIMER_EXPIRED:
        return "timer_expired";
    default:
        return "other";
    }
}

