#include "led_scene.h"

const char *led_scene_name(led_scene_id_t scene_id)
{
    switch (scene_id) {
    case LED_SCENE_IDLE_BREATHE:
        return "idle_breathe";
    case LED_SCENE_TOUCH_AWAKE:
        return "touch_awake";
    case LED_SCENE_ERROR_FLASH:
        return "error_flash";
    case LED_SCENE_FIRE2012:
        return "fire2012";
    case LED_SCENE_PLASMA:
        return "plasma";
    case LED_SCENE_SPARKLE:
        return "sparkle";
    case LED_SCENE_COLOR_WAVE:
        return "color_wave";
    case LED_SCENE_AURA_COLOR_BREATHE:
        return "aura_color_breathe";
    case LED_SCENE_GRUMBLE_RED:
        return "grumble_red";
    case LED_SCENE_LOTTERY_IDLE:
        return "lottery_idle";
    case LED_SCENE_LOTTERY_HOLD_RAMP:
        return "lottery_hold_ramp";
    case LED_SCENE_HYBRID_IDLE_SLOW_BREATHE:
        return "hybrid_idle_slow_breathe";
    case LED_SCENE_HYBRID_TOUCH_FAST_BREATHE:
        return "hybrid_touch_fast_breathe";
    case LED_SCENE_HYBRID_VORTEX:
        return "hybrid_vortex";
    case LED_SCENE_HYBRID_VORTEX_DIM:
        return "hybrid_vortex_dim";
    default:
        return "unknown_scene";
    }
}
