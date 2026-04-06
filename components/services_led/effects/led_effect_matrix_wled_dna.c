#include "led_effect_matrix_wled.h"

#include <math.h>
#include "sdkconfig.h"

#define LED_MATRIX_W ((uint32_t)CONFIG_ORB_LED_MATRIX_WIDTH)
#define LED_MATRIX_H ((uint32_t)CONFIG_ORB_LED_MATRIX_HEIGHT)

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

void led_effect_matrix_wled_dna_render(uint32_t now_ms,
                                       uint8_t brightness,
                                       uint8_t speed,
                                       uint8_t intensity,
                                       uint8_t scale,
                                       led_effects_set_pixel_fn set_pixel,
                                       void *ctx)
{
    if (set_pixel == NULL) {
        return;
    }

    const float t = (float)now_ms * (0.0013f + ((float)speed / 255.0f) * 0.0032f);
    const float amp = 0.15f + ((float)intensity / 255.0f) * 0.45f;
    const float freq = 0.7f + ((float)scale / 255.0f) * 3.2f;
    const float phase_step = 0.22f + ((float)scale / 255.0f) * 0.45f;

    for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
        float fy = (float)y / (float)(LED_MATRIX_H > 1U ? (LED_MATRIX_H - 1U) : 1U);
        float ph = t + fy * phase_step * 6.28318530718f;
        float x_center = 0.5f;
        float lane = amp * sinf(ph * freq);

        float lx = x_center + lane;
        float rx = x_center - lane;

        for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
            float fx = (float)x / (float)(LED_MATRIX_W > 1U ? (LED_MATRIX_W - 1U) : 1U);
            float dl = fabsf(fx - lx);
            float dr = fabsf(fx - rx);
            float d = (dl < dr) ? dl : dr;

            float core = smoothstepf(1.0f - d / 0.095f);
            float glow = smoothstepf(1.0f - d / 0.23f) * 0.35f;
            float mix = clamp01f(core + glow);

            uint8_t v = (uint8_t)(mix * 255.0f + 0.5f);
            uint8_t hue = (uint8_t)((uint32_t)((float)now_ms * 0.045f + fy * 120.0f + d * 220.0f) & 0xFFU);
            uint8_t r = 0U;
            uint8_t g = 0U;
            uint8_t b = 0U;
            if (v > 2U) {
                hsv_to_rgb_u8(hue, 236U, v, &r, &g, &b);
            }
            set_pixel(x, y, r, g, b, brightness, ctx);
        }
    }
}
