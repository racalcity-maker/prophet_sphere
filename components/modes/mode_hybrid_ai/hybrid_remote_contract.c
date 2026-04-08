#include "mode_hybrid_ai_internal.h"

bool hybrid_remote_event_is_bg_fade_done(const app_mode_event_t *event)
{
    return event != NULL &&
           event->id == APP_MODE_EVENT_AUDIO_DONE &&
           event->code == (int32_t)APP_MODE_AUDIO_DONE_CODE_BG_FADE_COMPLETE;
}

bool hybrid_remote_waiting_tts(hybrid_flow_t flow)
{
    return flow == HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE ||
           flow == HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE ||
           flow == HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE;
}

bool hybrid_remote_capture_matches(uint32_t event_capture_id, uint32_t active_capture_id)
{
    return event_capture_id == 0U || event_capture_id == active_capture_id;
}

uint32_t hybrid_remote_stream_timeout_ms(hybrid_flow_t flow)
{
    if (flow == HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE || flow == HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE) {
        return HYBRID_REMOTE_INTRO_STREAM_MAX_MS;
    }
    return HYBRID_REMOTE_STREAM_MAX_MS;
}

