#include "led_effects_scene_dispatch.h"

#include <math.h>
#include "sdkconfig.h"

#define LED_MATRIX_W ((uint32_t)CONFIG_ORB_LED_MATRIX_WIDTH)
#define LED_MATRIX_H ((uint32_t)CONFIG_ORB_LED_MATRIX_HEIGHT)

static uint8_t lerp_u8(uint8_t a, uint8_t b, uint8_t t)
{
    uint16_t inv = (uint16_t)(255U - t);
    uint16_t v = (uint16_t)(((uint32_t)a * inv + (uint32_t)b * t) / 255U);
    return (uint8_t)v;
}

static uint32_t hash_u32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352DU;
    x ^= x >> 15;
    x *= 0x846CA68BU;
    x ^= x >> 16;
    return x;
}

static uint16_t hybrid_breathe_wave_u16(uint32_t now_ms, uint32_t period_ms)
{
    if (period_ms < 2U) {
        return 65535U;
    }

    const float phase = (float)(now_ms % period_ms) / (float)period_ms;
    float n = 0.0f;
    if (phase < 0.5f) {
        const float x = phase * 2.0f;
        n = 1.0f - powf(1.0f - x, 1.75f);
    } else {
        const float x = (phase - 0.5f) * 2.0f;
        n = 1.0f - (x * x * x);
    }

    if (n < 0.018f) {
        n = 0.0f;
    }

    uint32_t v = (uint32_t)(n * 65535.0f + 0.5f);
    return (v > 65535U) ? 65535U : (uint16_t)v;
}

static uint8_t cycle_hue_latched_at_floor(uint32_t now_ms,
                                          uint32_t period_ms,
                                          uint32_t seed,
                                          uint16_t wave_target_q8,
                                          uint32_t *latched_cycle,
                                          uint8_t *latched_hue)
{
    if (period_ms == 0U || latched_cycle == NULL || latched_hue == NULL) {
        return (uint8_t)(hash_u32(seed) & 0xFFU);
    }

    uint32_t cycle_idx = now_ms / period_ms;
    if (*latched_cycle == UINT32_MAX) {
        *latched_cycle = cycle_idx;
        *latched_hue = (uint8_t)(hash_u32(cycle_idx + seed) & 0xFFU);
        return *latched_hue;
    }

    if (cycle_idx != *latched_cycle && wave_target_q8 <= (2U << 8)) {
        *latched_cycle = cycle_idx;
        *latched_hue = (uint8_t)(hash_u32(cycle_idx + seed) & 0xFFU);
    }
    return *latched_hue;
}

static uint8_t smooth_q8_to_u8_dither(uint16_t *state_q8, uint16_t target_q8, uint32_t now_ms, uint8_t seed)
{
    if (state_q8 == NULL) {
        return (uint8_t)(target_q8 >> 8);
    }

    if (target_q8 <= (3U << 8)) {
        target_q8 = 0U;
    }

    int32_t diff = (int32_t)target_q8 - (int32_t)(*state_q8);
    if (diff != 0) {
        uint8_t state_u8 = (uint8_t)((*state_q8) >> 8);
        uint16_t alpha_q8 = 160U;
        if (diff > 0) {
            if (state_u8 < 20U) {
                alpha_q8 = 224U;
            } else if (state_u8 < 80U) {
                alpha_q8 = 182U;
            } else {
                alpha_q8 = 142U;
            }
        } else {
            if (state_u8 < 20U) {
                alpha_q8 = 255U;
            } else if (state_u8 < 64U) {
                alpha_q8 = 236U;
            } else if (state_u8 < 132U) {
                alpha_q8 = 204U;
            } else {
                alpha_q8 = 170U;
            }
        }

        int32_t step = (diff * (int32_t)alpha_q8) / 256;
        if (step == 0) {
            step = (diff > 0) ? 1 : -1;
        }

        int32_t next = (int32_t)(*state_q8) + step;
        if (next < 0) {
            next = 0;
        } else if (next > 65535) {
            next = 65535;
        }

        if (diff > -96 && diff < 96) {
            next = (int32_t)target_q8;
        }
        *state_q8 = (uint16_t)next;
    }

    uint8_t out = (uint8_t)((*state_q8) >> 8);
    uint8_t frac = (uint8_t)(*state_q8 & 0xFFU);
    if (out == 0U && frac < 48U) {
        return 0U;
    }
    uint8_t threshold = (uint8_t)((now_ms * 29U + (uint32_t)seed * 53U) & 0xFFU);
    if (frac > threshold && out < 255U) {
        out++;
    }
    return out;
}

static float clamp01f(float x)
{
    if (x < 0.0f) {
        return 0.0f;
    }
    if (x > 1.0f) {
        return 1.0f;
    }
    return x;
}

static float smoothstepf(float x)
{
    float t = clamp01f(x);
    return t * t * (3.0f - 2.0f * t);
}

static void hsv_to_rgb_u8(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0U) {
        *r = v;
        *g = v;
        *b = v;
        return;
    }

    uint8_t region = h / 43U;
    uint8_t remainder = (uint8_t)((h - region * 43U) * 6U);
    uint8_t p = (uint8_t)(((uint16_t)v * (255U - s)) / 255U);
    uint8_t q = (uint8_t)(((uint16_t)v * (255U - (((uint16_t)s * remainder) / 255U))) / 255U);
    uint8_t t = (uint8_t)(((uint16_t)v * (255U - (((uint16_t)s * (255U - remainder)) / 255U))) / 255U);

    switch (region) {
    case 0:
        *r = v;
        *g = t;
        *b = p;
        break;
    case 1:
        *r = q;
        *g = v;
        *b = p;
        break;
    case 2:
        *r = p;
        *g = v;
        *b = t;
        break;
    case 3:
        *r = p;
        *g = q;
        *b = v;
        break;
    case 4:
        *r = t;
        *g = p;
        *b = v;
        break;
    default:
        *r = v;
        *g = p;
        *b = q;
        break;
    }
}

static void render_hybrid_idle_slow_breathe(led_effects_state_t *state,
                                            uint32_t now_ms,
                                            uint8_t brightness,
                                            led_effects_fill_fn fill_color,
                                            void *ctx)
{
    const uint32_t period_ms = 7800U;
    uint16_t wave_u16 = hybrid_breathe_wave_u16(now_ms, period_ms);
    uint16_t v_target_q8 = (uint16_t)(((uint32_t)176U * (uint32_t)wave_u16) / 256U);
    uint8_t v = smooth_q8_to_u8_dither(&state->hybrid_idle_v_q8, v_target_q8, now_ms, 0x57U);
    uint8_t hue =
        cycle_hue_latched_at_floor(now_ms, period_ms, 0x27U, v_target_q8, &state->hybrid_idle_hue_cycle, &state->hybrid_idle_hue);
    uint8_t sat = (uint8_t)(236U + ((uint16_t)v * 19U) / 255U);
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
    hsv_to_rgb_u8(hue, sat, v, &r, &g, &b);
    fill_color(r, g, b, brightness, ctx);
}

static void render_hybrid_touch_fast_breathe(led_effects_state_t *state,
                                             uint32_t now_ms,
                                             uint8_t brightness,
                                             led_effects_fill_fn fill_color,
                                             void *ctx)
{
    const uint32_t period_ms = 1400U;
    uint16_t wave_u16 = hybrid_breathe_wave_u16(now_ms, period_ms);
    uint16_t v_target_q8 = (uint16_t)(((uint32_t)210U * (uint32_t)wave_u16) / 256U);
    uint8_t v = smooth_q8_to_u8_dither(&state->hybrid_touch_v_q8, v_target_q8, now_ms, 0x73U);
    uint8_t hue = cycle_hue_latched_at_floor(
        now_ms, period_ms, 0x39U, v_target_q8, &state->hybrid_touch_hue_cycle, &state->hybrid_touch_hue);
    uint8_t sat = (uint8_t)(242U + ((uint16_t)v * 13U) / 255U);
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
    hsv_to_rgb_u8(hue, sat, v, &r, &g, &b);
    fill_color(r, g, b, brightness, ctx);
}

static void render_hybrid_vortex(const led_effects_state_t *state,
                                 uint32_t now_ms,
                                 uint8_t brightness,
                                 led_effects_set_pixel_fn set_pixel,
                                 void *ctx,
                                 bool dimmed)
{
    const float cx = ((float)LED_MATRIX_W - 1.0f) * 0.5f;
    const float cy = ((float)LED_MATRIX_H - 1.0f) * 0.5f;
    const float max_r = sqrtf(cx * cx + cy * cy) + 0.001f;
    const float t = (float)now_ms * 0.0012f;

    for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
        for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
            float fx = (float)x - cx;
            float fy = (float)y - cy;
            float r_norm = sqrtf(fx * fx + fy * fy) / max_r;
            if (r_norm > 1.0f) {
                r_norm = 1.0f;
            }

            float a = atan2f(fy, fx);
            float v1 = sinf(10.0f * r_norm - t * 1.9f);
            float v2 = sinf(3.0f * a + t * 1.3f);
            float v3 = sinf((fx * 0.75f - fy * 0.62f) + t * 0.8f);
            float mix = (v1 + v2 + v3) / 3.0f;
            float n = 0.5f + 0.5f * mix;
            float edge = smoothstepf((1.0f - r_norm) / 0.14f);
            float intensity = clamp01f((0.16f + 0.84f * n) * edge);
            uint8_t val = (uint8_t)(intensity * 255.0f + 0.5f);
            uint8_t hue = (uint8_t)((uint32_t)(95.0f * n + 140.0f * r_norm + (float)now_ms * 0.04f) & 0xFFU);
            uint8_t r = 0U;
            uint8_t g = 0U;
            uint8_t b = 0U;
            if (val > 2U) {
                hsv_to_rgb_u8(hue, 245U, val, &r, &g, &b);
            }

            uint8_t aura = state->aura_level;
            if (aura > 0U) {
                r = lerp_u8(r, state->aura_r, aura);
                g = lerp_u8(g, state->aura_g, aura);
                b = lerp_u8(b, state->aura_b, aura);
            }

            if (dimmed) {
                r = (uint8_t)(((uint16_t)r * 128U) / 255U);
                g = (uint8_t)(((uint16_t)g * 128U) / 255U);
                b = (uint8_t)(((uint16_t)b * 128U) / 255U);
            }

            set_pixel(x, y, r, g, b, brightness, ctx);
        }
    }
}

bool led_effects_render_hybrid_core_scene(led_scene_id_t scene_id,
                                          led_effects_state_t *state,
                                          uint32_t now_ms,
                                          uint8_t brightness,
                                          led_effects_set_pixel_fn set_pixel,
                                          led_effects_fill_fn fill_color,
                                          void *ctx)
{
    if (state == NULL || set_pixel == NULL || fill_color == NULL) {
        return false;
    }

    switch (scene_id) {
    case LED_SCENE_HYBRID_IDLE_SLOW_BREATHE:
        render_hybrid_idle_slow_breathe(state, now_ms, brightness, fill_color, ctx);
        return true;
    case LED_SCENE_HYBRID_TOUCH_FAST_BREATHE:
        render_hybrid_touch_fast_breathe(state, now_ms, brightness, fill_color, ctx);
        return true;
    case LED_SCENE_HYBRID_VORTEX:
        render_hybrid_vortex(state, now_ms, brightness, set_pixel, ctx, false);
        return true;
    case LED_SCENE_HYBRID_VORTEX_DIM:
        render_hybrid_vortex(state, now_ms, brightness, set_pixel, ctx, true);
        return true;
    default:
        return false;
    }
}
