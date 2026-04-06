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
    case LED_SCENE_HYBRID_WLED_METABALLS:
        return "hybrid_wled_metaballs";
    case LED_SCENE_HYBRID_WLED_DNA:
        return "hybrid_wled_dna";
    case LED_SCENE_HYBRID_WLED_TWISTER:
        return "hybrid_wled_twister";
    case LED_SCENE_HYBRID_WLED_CHECKER_PULSE:
        return "hybrid_wled_checker_pulse";
    case LED_SCENE_HYBRID_WLED_RAIN:
        return "hybrid_wled_rain";
    case LED_SCENE_HYBRID_WLED_RADIAL_BURST:
        return "hybrid_wled_radial_burst";
    case LED_SCENE_HYBRID_WLED_TUNNEL:
        return "hybrid_wled_tunnel";
    case LED_SCENE_HYBRID_WLED_BANDS:
        return "hybrid_wled_bands";
    case LED_SCENE_HYBRID_WLED_STARFIELD:
        return "hybrid_wled_starfield";
    case LED_SCENE_HYBRID_WLED_CONFETTI:
        return "hybrid_wled_confetti";
    case LED_SCENE_HYBRID_WLED_LAVA:
        return "hybrid_wled_lava";
    case LED_SCENE_HYBRID_WLED_RINGS:
        return "hybrid_wled_rings";
    case LED_SCENE_HYBRID_WLED_NOISE:
        return "hybrid_wled_noise";
    case LED_SCENE_HYBRID_WLED_SCANNER:
        return "hybrid_wled_scanner";
    case LED_SCENE_HYBRID_WLED_ZIGZAG:
        return "hybrid_wled_zigzag";
    case LED_SCENE_HYBRID_WLED_AURORA:
        return "hybrid_wled_aurora";
    case LED_SCENE_HYBRID_WLED_PRISM:
        return "hybrid_wled_prism";
    case LED_SCENE_HYBRID_WLED_CLOUDS:
        return "hybrid_wled_clouds";
    case LED_SCENE_HYBRID_WLED_WAVEGRID:
        return "hybrid_wled_wavegrid";
    case LED_SCENE_HYBRID_WLED_HEARTBEAT:
        return "hybrid_wled_heartbeat";
    case LED_SCENE_HYBRID_WLED_PINWHEEL:
        return "hybrid_wled_pinwheel";
    case LED_SCENE_HYBRID_WLED_COMET:
        return "hybrid_wled_comet";
    default:
        return "unknown_scene";
    }
}

bool led_scene_is_known(led_scene_id_t scene_id)
{
    switch (scene_id) {
    case LED_SCENE_IDLE_BREATHE:
    case LED_SCENE_TOUCH_AWAKE:
    case LED_SCENE_ERROR_FLASH:
    case LED_SCENE_FIRE2012:
    case LED_SCENE_PLASMA:
    case LED_SCENE_SPARKLE:
    case LED_SCENE_COLOR_WAVE:
    case LED_SCENE_AURA_COLOR_BREATHE:
    case LED_SCENE_GRUMBLE_RED:
    case LED_SCENE_LOTTERY_IDLE:
    case LED_SCENE_LOTTERY_HOLD_RAMP:
    case LED_SCENE_HYBRID_IDLE_SLOW_BREATHE:
    case LED_SCENE_HYBRID_TOUCH_FAST_BREATHE:
    case LED_SCENE_HYBRID_VORTEX:
    case LED_SCENE_HYBRID_VORTEX_DIM:
    case LED_SCENE_HYBRID_WLED_METABALLS:
    case LED_SCENE_HYBRID_WLED_DNA:
    case LED_SCENE_HYBRID_WLED_TWISTER:
    case LED_SCENE_HYBRID_WLED_CHECKER_PULSE:
    case LED_SCENE_HYBRID_WLED_RAIN:
    case LED_SCENE_HYBRID_WLED_RADIAL_BURST:
    case LED_SCENE_HYBRID_WLED_TUNNEL:
    case LED_SCENE_HYBRID_WLED_BANDS:
    case LED_SCENE_HYBRID_WLED_STARFIELD:
    case LED_SCENE_HYBRID_WLED_CONFETTI:
    case LED_SCENE_HYBRID_WLED_LAVA:
    case LED_SCENE_HYBRID_WLED_RINGS:
    case LED_SCENE_HYBRID_WLED_NOISE:
    case LED_SCENE_HYBRID_WLED_SCANNER:
    case LED_SCENE_HYBRID_WLED_ZIGZAG:
    case LED_SCENE_HYBRID_WLED_AURORA:
    case LED_SCENE_HYBRID_WLED_PRISM:
    case LED_SCENE_HYBRID_WLED_CLOUDS:
    case LED_SCENE_HYBRID_WLED_WAVEGRID:
    case LED_SCENE_HYBRID_WLED_HEARTBEAT:
    case LED_SCENE_HYBRID_WLED_PINWHEEL:
    case LED_SCENE_HYBRID_WLED_COMET:
        return true;
    default:
        return false;
    }
}
