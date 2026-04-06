#include "led_effects_scene_dispatch.h"

#include <stddef.h>
#include "led_effect_matrix_wled.h"

bool led_effects_render_hybrid_wled_scene(led_scene_id_t scene_id,
                                          uint32_t now_ms,
                                          uint8_t brightness,
                                          uint8_t effect_speed,
                                          uint8_t effect_intensity,
                                          uint8_t effect_scale,
                                          led_effects_set_pixel_fn set_pixel,
                                          void *ctx)
{
    if (set_pixel == NULL) {
        return false;
    }

    switch (scene_id) {
    case LED_SCENE_HYBRID_WLED_METABALLS:
        led_effect_matrix_wled_metaballs_render(
            now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_DNA:
        led_effect_matrix_wled_dna_render(
            now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_TWISTER:
        led_effect_matrix_wled_extra_render(
            0U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_CHECKER_PULSE:
        led_effect_matrix_wled_extra_render(
            1U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_RAIN:
        led_effect_matrix_wled_extra_render(
            2U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_RADIAL_BURST:
        led_effect_matrix_wled_extra_render(
            3U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_TUNNEL:
        led_effect_matrix_wled_extra_render(
            4U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_BANDS:
        led_effect_matrix_wled_extra_render(
            5U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_STARFIELD:
        led_effect_matrix_wled_extra_render(
            6U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_CONFETTI:
        led_effect_matrix_wled_extra_render(
            7U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_LAVA:
        led_effect_matrix_wled_extra_render(
            8U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_RINGS:
        led_effect_matrix_wled_extra_render(
            9U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_NOISE:
        led_effect_matrix_wled_extra_render(
            10U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_SCANNER:
        led_effect_matrix_wled_extra_render(
            11U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_ZIGZAG:
        led_effect_matrix_wled_extra_render(
            12U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_AURORA:
        led_effect_matrix_wled_extra_render(
            13U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_PRISM:
        led_effect_matrix_wled_extra_render(
            14U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_CLOUDS:
        led_effect_matrix_wled_extra_render(
            15U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_WAVEGRID:
        led_effect_matrix_wled_extra_render(
            16U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_HEARTBEAT:
        led_effect_matrix_wled_extra_render(
            17U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_PINWHEEL:
        led_effect_matrix_wled_extra_render(
            18U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    case LED_SCENE_HYBRID_WLED_COMET:
        led_effect_matrix_wled_extra_render(
            19U, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx);
        return true;
    default:
        return false;
    }
}
