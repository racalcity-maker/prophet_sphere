#ifndef LED_EFFECT_MATRIX_WLED_H
#define LED_EFFECT_MATRIX_WLED_H

#include <stdint.h>
#include "led_effects.h"

#ifdef __cplusplus
extern "C" {
#endif

void led_effect_matrix_wled_metaballs_render(uint32_t now_ms,
                                             uint8_t brightness,
                                             uint8_t speed,
                                             uint8_t intensity,
                                             uint8_t scale,
                                             led_effects_set_pixel_fn set_pixel,
                                             void *ctx);

void led_effect_matrix_wled_dna_render(uint32_t now_ms,
                                       uint8_t brightness,
                                       uint8_t speed,
                                       uint8_t intensity,
                                       uint8_t scale,
                                       led_effects_set_pixel_fn set_pixel,
                                       void *ctx);

void led_effect_matrix_wled_extra_render(uint8_t variant,
                                         uint32_t now_ms,
                                         uint8_t brightness,
                                         uint8_t speed,
                                         uint8_t intensity,
                                         uint8_t scale,
                                         led_effects_set_pixel_fn set_pixel,
                                         void *ctx);

#ifdef __cplusplus
}
#endif

#endif
