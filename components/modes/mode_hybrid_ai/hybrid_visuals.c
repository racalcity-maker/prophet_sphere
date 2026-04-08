#include "mode_hybrid_ai_internal.h"

uint32_t hybrid_effect_scene_normalize(uint32_t scene_id, uint32_t fallback_scene_id)
{
    switch (scene_id) {
    case ORB_LED_SCENE_ID_HYBRID_IDLE_SLOW_BREATHE:
    case ORB_LED_SCENE_ID_HYBRID_TOUCH_FAST_BREATHE:
    case ORB_LED_SCENE_ID_HYBRID_VORTEX:
    case ORB_LED_SCENE_ID_HYBRID_VORTEX_DIM:
    case ORB_LED_SCENE_ID_HYBRID_WLED_METABALLS:
    case ORB_LED_SCENE_ID_HYBRID_WLED_DNA:
    case ORB_LED_SCENE_ID_HYBRID_WLED_TWISTER:
    case ORB_LED_SCENE_ID_HYBRID_WLED_CHECKER_PULSE:
    case ORB_LED_SCENE_ID_HYBRID_WLED_RAIN:
    case ORB_LED_SCENE_ID_HYBRID_WLED_RADIAL_BURST:
    case ORB_LED_SCENE_ID_HYBRID_WLED_TUNNEL:
    case ORB_LED_SCENE_ID_HYBRID_WLED_BANDS:
    case ORB_LED_SCENE_ID_HYBRID_WLED_STARFIELD:
    case ORB_LED_SCENE_ID_HYBRID_WLED_CONFETTI:
    case ORB_LED_SCENE_ID_HYBRID_WLED_LAVA:
    case ORB_LED_SCENE_ID_HYBRID_WLED_RINGS:
    case ORB_LED_SCENE_ID_HYBRID_WLED_NOISE:
    case ORB_LED_SCENE_ID_HYBRID_WLED_SCANNER:
    case ORB_LED_SCENE_ID_HYBRID_WLED_ZIGZAG:
    case ORB_LED_SCENE_ID_HYBRID_WLED_AURORA:
    case ORB_LED_SCENE_ID_HYBRID_WLED_PRISM:
    case ORB_LED_SCENE_ID_HYBRID_WLED_CLOUDS:
    case ORB_LED_SCENE_ID_HYBRID_WLED_WAVEGRID:
    case ORB_LED_SCENE_ID_HYBRID_WLED_HEARTBEAT:
    case ORB_LED_SCENE_ID_HYBRID_WLED_PINWHEEL:
    case ORB_LED_SCENE_ID_HYBRID_WLED_COMET:
        return scene_id;
    default:
        return fallback_scene_id;
    }
}

uint32_t hybrid_effect_scene_speak(const hybrid_runtime_cfg_t *runtime)
{
    if (runtime == NULL) {
        return HYBRID_SCENE_VORTEX_ID;
    }
    return hybrid_effect_scene_normalize(runtime->effect_talk_scene_id, HYBRID_SCENE_VORTEX_ID);
}

uint32_t hybrid_effect_scene_idle(const hybrid_runtime_cfg_t *runtime)
{
    if (runtime == NULL) {
        return HYBRID_SCENE_IDLE_ID;
    }
    return hybrid_effect_scene_normalize(runtime->effect_idle_scene_id, HYBRID_SCENE_IDLE_ID);
}

uint32_t hybrid_effect_scene_listen(const hybrid_runtime_cfg_t *runtime)
{
    uint32_t speak_scene = hybrid_effect_scene_speak(runtime);
    if (speak_scene == ORB_LED_SCENE_ID_HYBRID_VORTEX) {
        return ORB_LED_SCENE_ID_HYBRID_VORTEX_DIM;
    }
    return speak_scene;
}

uint32_t hybrid_scene_for_flow(hybrid_flow_t flow, const hybrid_runtime_cfg_t *runtime)
{
    switch (flow) {
    case HYBRID_FLOW_WAIT_MIC_DONE:
        return hybrid_effect_scene_listen(runtime);
    case HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE:
    case HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE:
    case HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE:
    case HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE:
    case HYBRID_FLOW_WAIT_BG_FADE_OUT:
        return hybrid_effect_scene_speak(runtime);
    case HYBRID_FLOW_IDLE:
    default:
        return hybrid_effect_scene_idle(runtime);
    }
}

void hybrid_set_flow_and_scene(hybrid_flow_t *flow,
                               hybrid_flow_t next_flow,
                               const hybrid_runtime_cfg_t *runtime,
                               app_mode_action_t *action)
{
    if (flow != NULL) {
        *flow = next_flow;
    }
    if (action != NULL) {
        action->led.scene_id = hybrid_scene_for_flow(next_flow, runtime);
    }
}

