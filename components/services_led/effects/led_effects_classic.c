#include "led_effects_scene_dispatch.h"

#include <math.h>
#include "sdkconfig.h"

#define LED_MATRIX_W ((uint32_t)CONFIG_ORB_LED_MATRIX_WIDTH)
#define LED_MATRIX_H ((uint32_t)CONFIG_ORB_LED_MATRIX_HEIGHT)

static uint32_t prng_next(led_effects_state_t *state)
{
    state->rng_state = state->rng_state * 1664525U + 1013904223U;
    return state->rng_state;
}

static uint8_t prng_u8(led_effects_state_t *state)
{
    return (uint8_t)(prng_next(state) >> 24);
}

static uint8_t prng_range_u8(led_effects_state_t *state, uint8_t min, uint8_t max)
{
    if (max <= min) {
        return min;
    }
    uint16_t span = (uint16_t)max - (uint16_t)min + 1U;
    return (uint8_t)(min + (prng_next(state) % span));
}

static uint8_t tri8(uint32_t value)
{
    uint32_t phase = value & 0x1FFU;
    return (phase < 256U) ? (uint8_t)phase : (uint8_t)(511U - phase);
}

static uint8_t triangle_wave_0_255(uint32_t now_ms, uint32_t period_ms)
{
    if (period_ms < 2U) {
        return 255U;
    }

    uint32_t half = period_ms / 2U;
    uint32_t phase = now_ms % period_ms;
    if (phase < half) {
        return (uint8_t)((phase * 255U) / half);
    }
    return (uint8_t)(((period_ms - phase) * 255U) / half);
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

static uint16_t sine_wave_u16(uint32_t now_ms, uint32_t period_ms)
{
    if (period_ms < 2U) {
        return 65535U;
    }

    float phase = (float)(now_ms % period_ms) / (float)period_ms;
    float c = cosf(phase * 6.28318530718f);
    float n = 0.5f - 0.5f * c;
    n = 1.0f - powf(1.0f - n, 1.8f);
    uint32_t v = (uint32_t)(n * 65535.0f + 0.5f);
    return (v > 65535U) ? 65535U : (uint16_t)v;
}

static uint8_t cycle_hue_random(uint32_t now_ms, uint32_t period_ms, uint32_t seed)
{
    if (period_ms == 0U) {
        return (uint8_t)(hash_u32(seed) & 0xFFU);
    }
    uint32_t cycle_idx = now_ms / period_ms;
    return (uint8_t)(hash_u32(cycle_idx + seed) & 0xFFU);
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

static void heat_to_rgb(uint8_t heat, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t t192 = (uint8_t)(((uint16_t)heat * 191U) / 255U);
    uint8_t heatramp = (uint8_t)((t192 & 0x3FU) << 2U);

    if (t192 > 0x80U) {
        *r = 255U;
        *g = 255U;
        *b = heatramp;
    } else if (t192 > 0x40U) {
        *r = 255U;
        *g = heatramp;
        *b = 0U;
    } else {
        *r = heatramp;
        *g = 0U;
        *b = 0U;
    }
}

static void render_idle_breathe(led_effects_state_t *state, uint32_t now_ms, uint8_t brightness, led_effects_fill_fn fill_color, void *ctx)
{
    const uint32_t period_ms = (uint32_t)CONFIG_ORB_LED_BREATHE_PERIOD_MS;
    uint16_t wave_u16 = sine_wave_u16(now_ms, period_ms);
    uint16_t v_target_q8 = (uint16_t)(((uint32_t)196U * (uint32_t)wave_u16) / 256U);
    uint8_t v = smooth_q8_to_u8_dither(&state->idle_v_q8, v_target_q8, now_ms, 0x41U);
    uint8_t hue = cycle_hue_random(now_ms, period_ms, 0x11U);
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
    hsv_to_rgb_u8(hue, 200U, v, &r, &g, &b);
    fill_color(r, g, b, brightness, ctx);
}

static void render_touch_awake(uint8_t brightness, led_effects_fill_fn fill_color, void *ctx)
{
    fill_color(36U, 72U, 120U, brightness, ctx);
}

static void render_error_flash(uint32_t now_ms, uint8_t brightness, led_effects_fill_fn fill_color, led_effects_clear_fn clear, void *ctx)
{
    uint32_t phase = (now_ms / 220U) % 2U;
    if (phase == 0U) {
        fill_color(255U, 20U, 0U, brightness, ctx);
    } else {
        clear(ctx);
    }
}

static void render_fire2012(led_effects_state_t *state, uint8_t brightness, led_effects_set_pixel_fn set_pixel, void *ctx)
{
    for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
        uint8_t cooling = (uint8_t)(((uint32_t)CONFIG_ORB_LED_FIRE_COOLING * 10U) / LED_MATRIX_H + 2U);

        for (uint32_t row = 0; row < LED_MATRIX_H; ++row) {
            uint32_t idx = row * LED_MATRIX_W + x;
            uint8_t cool = prng_range_u8(state, 0U, cooling);
            state->fire_heat[idx] = (state->fire_heat[idx] > cool) ? (state->fire_heat[idx] - cool) : 0U;
        }

        for (int32_t row = (int32_t)LED_MATRIX_H - 1; row >= 2; --row) {
            uint32_t idx = (uint32_t)row * LED_MATRIX_W + x;
            uint32_t below = (uint32_t)(row - 1) * LED_MATRIX_W + x;
            uint32_t below2 = (uint32_t)(row - 2) * LED_MATRIX_W + x;
            state->fire_heat[idx] =
                (uint8_t)(((uint16_t)state->fire_heat[below] + (uint16_t)state->fire_heat[below2] +
                           (uint16_t)state->fire_heat[below2]) /
                          3U);
        }

        if (prng_u8(state) < (uint8_t)CONFIG_ORB_LED_FIRE_SPARKING) {
            uint8_t spark_row_max = (LED_MATRIX_H < 8U) ? (uint8_t)(LED_MATRIX_H - 1U) : 7U;
            uint8_t spark_row = prng_range_u8(state, 0U, spark_row_max);
            uint32_t spark_idx = (uint32_t)spark_row * LED_MATRIX_W + x;
            uint8_t spark = prng_range_u8(state, 160U, 255U);
            uint16_t sum = (uint16_t)state->fire_heat[spark_idx] + (uint16_t)spark;
            state->fire_heat[spark_idx] = (sum > 255U) ? 255U : (uint8_t)sum;
        }

        for (uint32_t row = 0; row < LED_MATRIX_H; ++row) {
            uint8_t r = 0U;
            uint8_t g = 0U;
            uint8_t b = 0U;
            heat_to_rgb(state->fire_heat[row * LED_MATRIX_W + x], &r, &g, &b);
            uint32_t matrix_y = (LED_MATRIX_H - 1U) - row;
            set_pixel(x, matrix_y, r, g, b, brightness, ctx);
        }
    }
}

static void render_plasma(uint32_t now_ms, uint8_t brightness, led_effects_set_pixel_fn set_pixel, void *ctx)
{
    for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
        for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
            uint8_t v1 = tri8(x * 16U + now_ms / 4U);
            uint8_t v2 = tri8(y * 20U + now_ms / 5U);
            uint8_t v3 = tri8((x + y) * 12U + now_ms / 7U);
            uint8_t hue = (uint8_t)(((uint16_t)v1 + (uint16_t)v2 + (uint16_t)v3) / 3U);
            uint8_t val = (uint8_t)(100U + tri8(now_ms / 3U + x * 5U + y * 3U) / 2U);
            uint8_t r = 0U;
            uint8_t g = 0U;
            uint8_t b = 0U;
            hsv_to_rgb_u8(hue, 220U, val, &r, &g, &b);
            set_pixel(x, y, r, g, b, brightness, ctx);
        }
    }
}

static void render_sparkle(led_effects_state_t *state, uint8_t brightness, led_effects_set_pixel_fn set_pixel, void *ctx)
{
    for (uint32_t i = 0; i < LED_EFFECTS_PIXEL_COUNT; ++i) {
        state->sparkle_level[i] = (uint8_t)(((uint16_t)state->sparkle_level[i] * 225U) / 255U);
    }

    for (uint32_t i = 0; i < 4U; ++i) {
        if (prng_u8(state) < (uint8_t)CONFIG_ORB_LED_SPARKLE_DENSITY) {
            uint32_t idx = prng_next(state) % LED_EFFECTS_PIXEL_COUNT;
            state->sparkle_level[idx] = 255U;
        }
    }

    for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
        for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
            uint8_t base = (uint8_t)(14U + (y * 28U) / LED_MATRIX_H);
            set_pixel(x, y, (uint8_t)(base / 8U), (uint8_t)(base / 3U), base, brightness, ctx);
        }
    }

    for (uint32_t idx = 0; idx < LED_EFFECTS_PIXEL_COUNT; ++idx) {
        uint8_t sparkle = state->sparkle_level[idx];
        if (sparkle == 0U) {
            continue;
        }
        uint32_t x = idx % LED_MATRIX_W;
        uint32_t y = idx / LED_MATRIX_W;
        set_pixel(x, y, sparkle, sparkle, sparkle, brightness, ctx);
    }
}

static void render_color_wave(uint32_t now_ms, uint8_t brightness, led_effects_set_pixel_fn set_pixel, void *ctx)
{
    for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
        for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
            uint8_t hue = (uint8_t)((x * 11U + y * 3U + now_ms / 18U) & 0xFFU);
            uint8_t val = (uint8_t)(80U + (tri8(now_ms / 5U + y * 22U) + tri8(now_ms / 6U + x * 17U)) / 4U);
            uint8_t r = 0U;
            uint8_t g = 0U;
            uint8_t b = 0U;
            hsv_to_rgb_u8(hue, 200U, val, &r, &g, &b);
            set_pixel(x, y, r, g, b, brightness, ctx);
        }
    }
}

static void render_aura_color_breathe(const led_effects_state_t *state,
                                      uint32_t now_ms,
                                      uint8_t brightness,
                                      led_effects_fill_fn fill_color,
                                      void *ctx)
{
    uint8_t wave = triangle_wave_0_255(now_ms, (uint32_t)CONFIG_ORB_LED_BREATHE_PERIOD_MS);
    uint32_t min_level = 80U;
    uint32_t amp = 175U;
    uint8_t breathe_level = (uint8_t)(min_level + ((amp * wave) / 255U));

    uint8_t r = (uint8_t)(((uint32_t)state->aura_r * (uint32_t)breathe_level * (uint32_t)state->aura_level) / (255U * 255U));
    uint8_t g = (uint8_t)(((uint32_t)state->aura_g * (uint32_t)breathe_level * (uint32_t)state->aura_level) / (255U * 255U));
    uint8_t b = (uint8_t)(((uint32_t)state->aura_b * (uint32_t)breathe_level * (uint32_t)state->aura_level) / (255U * 255U));
    fill_color(r, g, b, brightness, ctx);
}

static void render_grumble_red(const led_effects_state_t *state,
                               uint8_t brightness,
                               led_effects_fill_fn fill_color,
                               void *ctx)
{
    uint8_t r = (uint8_t)(((uint32_t)255U * (uint32_t)state->aura_level) / 255U);
    fill_color(r, 0U, 0U, brightness, ctx);
}

static void render_lottery_idle(led_effects_state_t *state,
                                uint32_t now_ms,
                                uint8_t brightness,
                                led_effects_fill_fn fill_color,
                                void *ctx)
{
    if (state == NULL) {
        return;
    }

    uint8_t teams = state->lottery_team_count;
    if (teams < 2U) {
        teams = 2U;
    }
    if (teams > 4U) {
        teams = 4U;
    }

    const uint32_t breathe_cycle_ms = 2400U;
    uint32_t color_idx = (now_ms / breathe_cycle_ms) % teams;
    uint8_t base_r = state->lottery_color_r[color_idx];
    uint8_t base_g = state->lottery_color_g[color_idx];
    uint8_t base_b = state->lottery_color_b[color_idx];

    /* Reduce shared RGB floor to keep team colors vivid (less "white wash"). */
    uint8_t min_c = base_r;
    if (base_g < min_c) {
        min_c = base_g;
    }
    if (base_b < min_c) {
        min_c = base_b;
    }
    uint8_t cut = (uint8_t)(((uint16_t)min_c * 220U) / 255U);
    base_r = (base_r > cut) ? (uint8_t)(base_r - cut) : 0U;
    base_g = (base_g > cut) ? (uint8_t)(base_g - cut) : 0U;
    base_b = (base_b > cut) ? (uint8_t)(base_b - cut) : 0U;

    uint16_t wave_u16 = sine_wave_u16(now_ms, breathe_cycle_ms);
    uint16_t v_target_q8 = (uint16_t)((48U << 8) + (((uint32_t)172U * (uint32_t)wave_u16) / 256U));
    uint8_t v = smooth_q8_to_u8_dither(&state->idle_v_q8, v_target_q8, now_ms, (uint8_t)(0x30U + color_idx));

    uint8_t r = (uint8_t)(((uint16_t)base_r * (uint16_t)v) / 255U);
    uint8_t g = (uint8_t)(((uint16_t)base_g * (uint16_t)v) / 255U);
    uint8_t b = (uint8_t)(((uint16_t)base_b * (uint16_t)v) / 255U);
    fill_color(r, g, b, brightness, ctx);
}

static void render_lottery_hold_ramp(uint32_t scene_elapsed_ms, uint8_t brightness, led_effects_fill_fn fill_color, void *ctx)
{
    const uint32_t ramp_ms = 3000U;
    const uint8_t min_v = 40U;
    const uint8_t max_v = 255U;
    uint8_t v = max_v;

    if (scene_elapsed_ms < ramp_ms) {
        v = (uint8_t)(min_v + (((uint32_t)(max_v - min_v) * scene_elapsed_ms) / ramp_ms));
    }

    fill_color(v, v, v, brightness, ctx);
}

static void render_lottery_team_color(const led_effects_state_t *state,
                                      uint8_t brightness,
                                      led_effects_fill_fn fill_color,
                                      void *ctx)
{
    if (state == NULL) {
        return;
    }
    fill_color(state->aura_r, state->aura_g, state->aura_b, brightness, ctx);
}

bool led_effects_render_classic_scene(led_scene_id_t scene_id,
                                      led_effects_state_t *state,
                                      uint32_t now_ms,
                                      uint32_t scene_elapsed_ms,
                                      uint8_t brightness,
                                      led_effects_set_pixel_fn set_pixel,
                                      led_effects_fill_fn fill_color,
                                      led_effects_clear_fn clear,
                                      void *ctx)
{
    if (state == NULL || set_pixel == NULL || fill_color == NULL || clear == NULL) {
        return false;
    }

    switch (scene_id) {
    case LED_SCENE_IDLE_BREATHE:
        render_idle_breathe(state, now_ms, brightness, fill_color, ctx);
        return true;
    case LED_SCENE_TOUCH_AWAKE:
        render_touch_awake(brightness, fill_color, ctx);
        return true;
    case LED_SCENE_ERROR_FLASH:
        render_error_flash(now_ms, brightness, fill_color, clear, ctx);
        return true;
    case LED_SCENE_FIRE2012:
        render_fire2012(state, brightness, set_pixel, ctx);
        return true;
    case LED_SCENE_PLASMA:
        render_plasma(now_ms, brightness, set_pixel, ctx);
        return true;
    case LED_SCENE_SPARKLE:
        render_sparkle(state, brightness, set_pixel, ctx);
        return true;
    case LED_SCENE_COLOR_WAVE:
        render_color_wave(now_ms, brightness, set_pixel, ctx);
        return true;
    case LED_SCENE_AURA_COLOR_BREATHE:
        render_aura_color_breathe(state, now_ms, brightness, fill_color, ctx);
        return true;
    case LED_SCENE_GRUMBLE_RED:
        render_grumble_red(state, brightness, fill_color, ctx);
        return true;
    case LED_SCENE_LOTTERY_IDLE:
        render_lottery_idle(state, now_ms, brightness, fill_color, ctx);
        return true;
    case LED_SCENE_LOTTERY_HOLD_RAMP:
        render_lottery_hold_ramp(scene_elapsed_ms, brightness, fill_color, ctx);
        return true;
    case LED_SCENE_LOTTERY_TEAM_COLOR:
        render_lottery_team_color(state, brightness, fill_color, ctx);
        return true;
    default:
        return false;
    }
}
