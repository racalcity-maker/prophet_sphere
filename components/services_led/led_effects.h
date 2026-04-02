#ifndef LED_EFFECTS_H
#define LED_EFFECTS_H

#include <stdint.h>
#include "sdkconfig.h"
#include "led_scene.h"

#define LED_EFFECTS_PIXEL_COUNT ((uint32_t)(CONFIG_ORB_LED_MATRIX_WIDTH * CONFIG_ORB_LED_MATRIX_HEIGHT))

typedef struct {
    uint8_t fire_heat[LED_EFFECTS_PIXEL_COUNT];
    uint8_t sparkle_level[LED_EFFECTS_PIXEL_COUNT];
    uint8_t aura_r;
    uint8_t aura_g;
    uint8_t aura_b;
    uint8_t aura_level;
    uint32_t rng_state;
} led_effects_state_t;

typedef void (*led_effects_set_pixel_fn)(uint32_t x,
                                         uint32_t y,
                                         uint8_t r,
                                         uint8_t g,
                                         uint8_t b,
                                         uint8_t brightness,
                                         void *ctx);

typedef void (*led_effects_fill_fn)(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness, void *ctx);
typedef void (*led_effects_clear_fn)(void *ctx);

void led_effects_reset_state(led_effects_state_t *state, uint32_t seed);

void led_effects_render_scene(led_scene_id_t scene_id,
                              led_effects_state_t *state,
                              uint32_t now_ms,
                              uint32_t scene_elapsed_ms,
                              uint8_t brightness,
                              led_effects_set_pixel_fn set_pixel,
                              led_effects_fill_fn fill_color,
                              led_effects_clear_fn clear,
                              void *ctx);

#endif
