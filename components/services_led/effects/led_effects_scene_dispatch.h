#ifndef LED_EFFECTS_SCENE_DISPATCH_H
#define LED_EFFECTS_SCENE_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>
#include "led_effects.h"

bool led_effects_render_classic_scene(led_scene_id_t scene_id,
                                      led_effects_state_t *state,
                                      uint32_t now_ms,
                                      uint32_t scene_elapsed_ms,
                                      uint8_t brightness,
                                      led_effects_set_pixel_fn set_pixel,
                                      led_effects_fill_fn fill_color,
                                      led_effects_clear_fn clear,
                                      void *ctx);

bool led_effects_render_hybrid_core_scene(led_scene_id_t scene_id,
                                          led_effects_state_t *state,
                                          uint32_t now_ms,
                                          uint8_t brightness,
                                          led_effects_set_pixel_fn set_pixel,
                                          led_effects_fill_fn fill_color,
                                          void *ctx);

bool led_effects_render_hybrid_wled_scene(led_scene_id_t scene_id,
                                          uint32_t now_ms,
                                          uint8_t brightness,
                                          uint8_t effect_speed,
                                          uint8_t effect_intensity,
                                          uint8_t effect_scale,
                                          led_effects_set_pixel_fn set_pixel,
                                          void *ctx);

#endif
