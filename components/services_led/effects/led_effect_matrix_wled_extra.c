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

static float fractf_local(float x)
{
    return x - floorf(x);
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

static void set_hsv_px(uint32_t x,
                       uint32_t y,
                       uint8_t hue,
                       float val01,
                       uint8_t brightness,
                       led_effects_set_pixel_fn set_pixel,
                       void *ctx)
{
    if (val01 <= 0.003f) {
        set_pixel(x, y, 0U, 0U, 0U, brightness, ctx);
        return;
    }
    uint8_t v = (uint8_t)(clamp01f(val01) * 255.0f + 0.5f);
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
    hsv_to_rgb_u8(hue, 240U, v, &r, &g, &b);
    set_pixel(x, y, r, g, b, brightness, ctx);
}

void led_effect_matrix_wled_extra_render(uint8_t variant,
                                         uint32_t now_ms,
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

    const float w = (float)(LED_MATRIX_W > 1U ? (LED_MATRIX_W - 1U) : 1U);
    const float h = (float)(LED_MATRIX_H > 1U ? (LED_MATRIX_H - 1U) : 1U);
    const float nx_scale = 2.0f / w;
    const float ny_scale = 2.0f / h;
    const float t = (float)now_ms * (0.00045f + ((float)speed / 255.0f) * 0.0032f);
    const float s = 0.5f + ((float)scale / 255.0f) * 3.7f;
    const float in = 0.45f + ((float)intensity / 255.0f) * 1.75f;

    for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
        for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
            const float fx = (float)x;
            const float fy = (float)y;
            const float nx = fx * nx_scale - 1.0f;
            const float ny = fy * ny_scale - 1.0f;
            const float r = sqrtf(nx * nx + ny * ny);
            const float a = atan2f(ny, nx);

            float v = 0.0f;
            float huef = 0.0f;

            switch (variant) {
            case 0: /* twister */
            {
                float cx = 0.65f * sinf(ny * 3.4f * s + t * 2.2f);
                float d = fabsf(nx - cx);
                v = clamp01f((0.34f - d) * 3.0f * in);
                huef = 140.0f + 90.0f * sinf(t + ny * 2.0f);
                break;
            }
            case 1: /* checker pulse */
            {
                uint32_t cell = (((uint32_t)(x / 2U) + (uint32_t)(y / 2U) + (uint32_t)(t * 3.0f)) & 1U);
                float pulse = 0.35f + 0.65f * (0.5f + 0.5f * sinf(t * 5.0f));
                v = cell ? pulse : pulse * 0.22f;
                huef = 12.0f + fx * 9.0f + fy * 7.0f;
                break;
            }
            case 2: /* rain */
            {
                uint32_t hh = hash_u32(x * 2654435761U);
                float col_phase = (float)(hh & 0xFFU) / 255.0f;
                float head = fmodf(t * (4.0f + in * 4.0f) + col_phase * (h + 1.0f), h + 1.0f);
                float d = fabsf(fy - head);
                d = fminf(d, (h + 1.0f) - d);
                v = clamp01f((1.8f - d) * 0.65f);
                huef = 150.0f + col_phase * 70.0f;
                break;
            }
            case 3: /* radial burst */
                v = clamp01f((0.5f + 0.5f * sinf(r * 20.0f * s - t * 8.0f)) * (1.25f - r) * in);
                huef = 200.0f + r * 140.0f - t * 40.0f;
                break;
            case 4: /* tunnel */
                v = clamp01f((0.5f + 0.5f * sinf(18.0f * r * s - t * 6.0f + 4.0f * a)) * (1.1f - r));
                huef = 110.0f + 80.0f * sinf(3.0f * a + t);
                break;
            case 5: /* bands */
                v = clamp01f(0.5f + 0.5f * sinf(ny * 16.0f * s + t * 5.0f));
                v *= (0.35f + in * 0.5f);
                huef = 170.0f + ny * 90.0f + t * 20.0f;
                break;
            case 6: /* starfield */
            {
                uint32_t frame = now_ms / (80U + (uint32_t)(255U - speed));
                uint32_t hh = hash_u32(x * 73856093U ^ y * 19349663U ^ frame * 83492791U);
                float p = (float)(hh & 1023U) / 1023.0f;
                v = (p > (0.985f - 0.07f * ((float)intensity / 255.0f))) ? 1.0f : 0.0f;
                v *= (0.6f + 0.4f * (0.5f + 0.5f * sinf(t * 4.0f + r * 7.0f)));
                huef = 180.0f + (float)((hh >> 10) & 63U);
                break;
            }
            case 7: /* confetti */
            {
                uint32_t frame = now_ms / (45U + (uint32_t)(180U - speed / 2U));
                uint32_t hh = hash_u32(x * 1640531513U + y * 2654435761U + frame * 97531U);
                float p = (float)(hh & 2047U) / 2047.0f;
                v = (p > (0.965f - 0.08f * ((float)intensity / 255.0f))) ? 1.0f : 0.0f;
                huef = (float)((hh >> 11) & 255U);
                break;
            }
            case 8: /* lava */
                v = clamp01f(0.5f + 0.28f * sinf(nx * 6.0f * s + t * 1.5f) + 0.28f * sinf(ny * 4.0f * s - t * 1.7f));
                v = powf(v, 1.35f);
                huef = 8.0f + 24.0f * v;
                break;
            case 9: /* rings */
                v = clamp01f(0.5f + 0.5f * cosf(r * 22.0f * s - t * 7.0f));
                v *= clamp01f(1.15f - r);
                huef = 120.0f + 100.0f * sinf(r * 8.0f - t);
                break;
            case 10: /* noise */
            {
                float qx = nx * 1.6f * s + t * 0.9f;
                float qy = ny * 1.6f * s - t * 0.8f;
                uint32_t hh = hash_u32((uint32_t)(qx * 8192.0f) * 92837111U ^ (uint32_t)(qy * 8192.0f) * 689287499U);
                v = ((float)(hh & 255U) / 255.0f);
                v = powf(v, 1.1f);
                huef = (float)((hh >> 8) & 255U);
                break;
            }
            case 11: /* scanner */
            {
                float pos = -1.0f + 2.0f * (0.5f + 0.5f * sinf(t * 2.8f));
                float d = fabsf(nx - pos);
                v = clamp01f((0.42f - d) * 2.8f * in) + clamp01f((0.8f - d) * 0.22f);
                huef = 195.0f;
                break;
            }
            case 12: /* zigzag */
            {
                float zz = sinf((nx + ny) * 8.0f * s + t * 4.2f) * cosf((nx - ny) * 5.0f * s - t * 3.3f);
                v = clamp01f(0.5f + 0.5f * zz);
                huef = 70.0f + 150.0f * v + t * 12.0f;
                break;
            }
            case 13: /* aurora */
            {
                float curtain = sinf(nx * 3.0f + t * 0.9f) + sinf(nx * 7.0f - t * 1.5f);
                v = clamp01f((0.42f + 0.28f * curtain) * (0.55f + 0.45f * (0.5f + 0.5f * sinf(ny * 10.0f + t * 2.1f))));
                huef = 105.0f + 45.0f * sinf(nx * 2.0f + t * 0.6f);
                break;
            }
            case 14: /* prism */
                v = clamp01f(0.25f + 0.75f * (0.5f + 0.5f * sinf((nx * 12.0f + ny * 7.0f) * s + t * 5.4f)));
                huef = 255.0f * fractf_local((a + 3.1415926f) / 6.2831853f + t * 0.03f);
                break;
            case 15: /* clouds */
            {
                float n1 = sinf(nx * 3.0f * s + t * 0.8f) + sinf(ny * 2.0f * s - t * 0.7f);
                float n2 = sinf((nx + ny) * 2.4f * s + t * 0.5f);
                v = clamp01f(0.52f + 0.2f * n1 + 0.18f * n2);
                v = powf(v, 1.15f);
                huef = 145.0f + 20.0f * n2;
                break;
            }
            case 16: /* wavegrid */
            {
                float gx = 0.5f + 0.5f * sinf(nx * 14.0f * s + t * 6.0f);
                float gy = 0.5f + 0.5f * sinf(ny * 14.0f * s - t * 5.5f);
                v = clamp01f(gx * gy * 1.4f);
                huef = 160.0f + 80.0f * gx;
                break;
            }
            case 17: /* heartbeat */
            {
                float beat = fabsf(sinf(t * 1.75f));
                beat = powf(beat, 22.0f);
                float glow = clamp01f((0.48f - r) * 2.8f);
                v = clamp01f(beat * in + glow * (0.25f + 0.75f * beat));
                huef = 236.0f - 18.0f * beat;
                break;
            }
            case 18: /* pinwheel */
            {
                float spokes = sinf(a * (4.0f + 4.0f * ((float)intensity / 255.0f)) - t * (2.0f + 6.0f * ((float)speed / 255.0f)));
                float edge = clamp01f(1.2f - r);
                v = clamp01f((0.5f + 0.5f * spokes) * edge);
                huef = 255.0f * fractf_local((a + 3.1415926f) / 6.2831853f + t * 0.04f);
                break;
            }
            case 19: /* comet */
            default:
            {
                float headx = -1.15f + 2.3f * fractf_local(t * 0.28f);
                float heady = 0.65f * sinf(t * 0.9f);
                float dx = nx - headx;
                float dy = ny - heady;
                float tail = clamp01f(1.0f - (dx * 0.9f + dy * 0.3f + 0.35f));
                float dist = sqrtf(dx * dx + dy * dy);
                float core = clamp01f((0.32f - dist) * 3.3f);
                v = clamp01f(core + tail * 0.55f);
                huef = 185.0f + 60.0f * tail;
                break;
            }
            }

            set_hsv_px(x, y, (uint8_t)((uint32_t)huef & 0xFFU), v, brightness, set_pixel, ctx);
        }
    }
}

