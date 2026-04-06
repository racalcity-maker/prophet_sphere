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

void led_effect_matrix_wled_metaballs_render(uint32_t now_ms,
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

    const float w = (float)LED_MATRIX_W;
    const float h = (float)LED_MATRIX_H;
    const float t = ((float)now_ms * (0.00045f + ((float)speed / 255.0f) * 0.0018f));
    const float blob_pull = 1.2f + ((float)intensity / 255.0f) * 5.0f;
    const float color_flow = 0.7f + ((float)scale / 255.0f) * 2.6f;

    const float bx0 = (w - 1.0f) * (0.5f + 0.42f * sinf(t * 0.93f));
    const float by0 = (h - 1.0f) * (0.5f + 0.42f * cosf(t * 1.17f));
    const float bx1 = (w - 1.0f) * (0.5f + 0.41f * cosf(t * 1.31f + 1.6f));
    const float by1 = (h - 1.0f) * (0.5f + 0.41f * sinf(t * 1.07f + 0.9f));
    const float bx2 = (w - 1.0f) * (0.5f + 0.38f * sinf(t * 1.59f + 2.2f));
    const float by2 = (h - 1.0f) * (0.5f + 0.38f * cosf(t * 1.43f + 0.2f));

    for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
        for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
            float fx = (float)x;
            float fy = (float)y;

            float dx0 = fx - bx0;
            float dy0 = fy - by0;
            float dx1 = fx - bx1;
            float dy1 = fy - by1;
            float dx2 = fx - bx2;
            float dy2 = fy - by2;

            float d0 = dx0 * dx0 + dy0 * dy0 + 0.25f;
            float d1 = dx1 * dx1 + dy1 * dy1 + 0.25f;
            float d2 = dx2 * dx2 + dy2 * dy2 + 0.25f;

            float field = blob_pull * (1.0f / d0 + 1.0f / d1 + 1.0f / d2);
            float n = clamp01f(field * 0.22f);

            uint8_t v = (uint8_t)(n * 255.0f + 0.5f);
            uint8_t hue = (uint8_t)((uint32_t)(
                (float)now_ms * 0.03f * color_flow +
                (float)x * (2.0f + 8.0f * ((float)scale / 255.0f)) +
                (float)y * 3.0f) & 0xFFU);

            uint8_t r = 0U;
            uint8_t g = 0U;
            uint8_t b = 0U;
            if (v > 2U) {
                hsv_to_rgb_u8(hue, 240U, v, &r, &g, &b);
            }
            set_pixel(x, y, r, g, b, brightness, ctx);
        }
    }
}
