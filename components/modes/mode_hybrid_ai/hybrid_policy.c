#include "mode_hybrid_ai_internal.h"

#include "config_manager.h"

uint8_t hybrid_unknown_retry_limit(const hybrid_runtime_cfg_t *runtime)
{
    uint32_t v = (runtime != NULL) ? (uint32_t)runtime->unknown_retry_max : 0U;
    if (v > 2U) {
        v = 2U;
    }
    return (uint8_t)v;
}

bool hybrid_intent_is_unknown_like(uint8_t intent_id)
{
    return intent_id == ORB_INTENT_UNKNOWN || intent_id == ORB_INTENT_UNCERTAIN;
}

void hybrid_policy_load_runtime_cfg(hybrid_runtime_cfg_t *runtime)
{
    orb_runtime_config_t snapshot = { 0 };
    if (runtime == NULL) {
        return;
    }
    if (config_manager_get_snapshot(&snapshot) != ESP_OK) {
        return;
    }

    runtime->bg_fade_in_ms = snapshot.prophecy_bg_fade_in_ms;
    runtime->bg_fade_out_ms = snapshot.prophecy_bg_fade_out_ms;
    runtime->bg_gain_permille = snapshot.prophecy_bg_gain_permille;
    runtime->unknown_retry_max = (snapshot.hybrid_unknown_retry_max <= 2U)
                                     ? snapshot.hybrid_unknown_retry_max
                                     : 2U;
    runtime->effect_idle_scene_id = hybrid_effect_scene_normalize(snapshot.hybrid_effect_idle_scene_id, HYBRID_SCENE_IDLE_ID);
    runtime->effect_talk_scene_id = hybrid_effect_scene_normalize(snapshot.hybrid_effect_talk_scene_id, HYBRID_SCENE_VORTEX_ID);
    runtime->effect_speed = snapshot.hybrid_effect_speed;
    runtime->effect_intensity = snapshot.hybrid_effect_intensity;
    runtime->effect_scale = snapshot.hybrid_effect_scale;

    if (snapshot.hybrid_mic_capture_ms >= 1000U && snapshot.hybrid_mic_capture_ms <= 60000U) {
        runtime->mic_capture_ms = snapshot.hybrid_mic_capture_ms;
    } else {
        runtime->mic_capture_ms = HYBRID_MIC_CAPTURE_MS_DEFAULT;
    }
}

void hybrid_resolve_intent_color(uint8_t intent_id, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r == NULL || g == NULL || b == NULL) {
        return;
    }

    switch (intent_id) {
    case ORB_INTENT_LOVE:
        *r = 255U;
        *g = 70U;
        *b = 170U;
        break;
    case ORB_INTENT_DANGER:
        *r = 255U;
        *g = 25U;
        *b = 20U;
        break;
    case ORB_INTENT_PATH:
        *r = 255U;
        *g = 215U;
        *b = 40U;
        break;
    case ORB_INTENT_MONEY:
        *r = 40U;
        *g = 255U;
        *b = 90U;
        break;
    case ORB_INTENT_CHOICE:
        *r = 70U;
        *g = 220U;
        *b = 255U;
        break;
    case ORB_INTENT_FUTURE:
        *r = 145U;
        *g = 100U;
        *b = 255U;
        break;
    case ORB_INTENT_INNER_STATE:
        *r = 70U;
        *g = 130U;
        *b = 255U;
        break;
    case ORB_INTENT_WISH:
        *r = 220U;
        *g = 80U;
        *b = 255U;
        break;
    case ORB_INTENT_TIME:
        *r = 255U;
        *g = 155U;
        *b = 40U;
        break;
    case ORB_INTENT_PAST:
        *r = 70U;
        *g = 180U;
        *b = 180U;
        break;
    case ORB_INTENT_PLACE:
        *r = 255U;
        *g = 180U;
        *b = 80U;
        break;
    case ORB_INTENT_FORBIDDEN:
        *r = 255U;
        *g = 32U;
        *b = 32U;
        break;
    case ORB_INTENT_JOKE:
        *r = 255U;
        *g = 255U;
        *b = 90U;
        break;
    case ORB_INTENT_UNCERTAIN:
    case ORB_INTENT_UNKNOWN:
    default:
        *r = 170U;
        *g = 200U;
        *b = 255U;
        break;
    }
}

