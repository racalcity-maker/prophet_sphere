#include "led_effects.h"

#include <stddef.h>
#include "led_effects_scene_dispatch.h"

void led_effects_render_scene(led_scene_id_t scene_id,
                              led_effects_state_t *state,
                              uint32_t now_ms,
                              uint32_t scene_elapsed_ms,
                              uint8_t effect_speed,
                              uint8_t effect_intensity,
                              uint8_t effect_scale,
                              uint8_t brightness,
                              led_effects_set_pixel_fn set_pixel,
                              led_effects_fill_fn fill_color,
                              led_effects_clear_fn clear,
                              void *ctx)
{
    if (state == NULL || set_pixel == NULL || fill_color == NULL || clear == NULL) {
        return;
    }

    clear(ctx);

    if (led_effects_render_classic_scene(
            scene_id, state, now_ms, scene_elapsed_ms, brightness, set_pixel, fill_color, clear, ctx)) {
        return;
    }

    if (led_effects_render_hybrid_core_scene(scene_id, state, now_ms, brightness, set_pixel, fill_color, ctx)) {
        return;
    }

    if (led_effects_render_hybrid_wled_scene(
            scene_id, now_ms, brightness, effect_speed, effect_intensity, effect_scale, set_pixel, ctx)) {
        return;
    }

    (void)led_effects_render_classic_scene(
        LED_SCENE_IDLE_BREATHE, state, now_ms, scene_elapsed_ms, brightness, set_pixel, fill_color, clear, ctx);
}
