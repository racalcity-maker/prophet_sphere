#include "led_task_internal.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "led_power_limit.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_LED;
extern uint32_t s_last_limit_log_ms;

static uint8_t scale_u8(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * (uint16_t)brightness) / 255U);
}

static uint8_t scale_u8_permille(uint8_t value, uint16_t gain_permille)
{
    return (uint8_t)(((uint32_t)value * gain_permille) / 1000U);
}

static uint8_t add_sat_u8(uint8_t value, uint8_t add)
{
    uint16_t sum = (uint16_t)value + (uint16_t)add;
    return (sum > 255U) ? 255U : (uint8_t)sum;
}

static uint8_t lerp_u8(uint8_t a, uint8_t b, uint8_t t)
{
    uint16_t inv = (uint16_t)(255U - t);
    return (uint8_t)(((uint16_t)a * inv + (uint16_t)b * t) / 255U);
}

bool led_task_palette_mode_valid(uint8_t mode)
{
    return mode == LED_PALETTE_MODE_RAINBOW || mode == LED_PALETTE_MODE_DUO || mode == LED_PALETTE_MODE_TRIO;
}

static uint8_t rgb_hue_u8(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t max = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    uint8_t min = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
    uint8_t delta = max - min;
    if (delta == 0U) {
        return 0U;
    }
    int16_t hue = 0;
    if (max == r) {
        hue = (int16_t)(43 * (int16_t)(g - b) / (int16_t)delta);
    } else if (max == g) {
        hue = (int16_t)(85 + 43 * (int16_t)(b - r) / (int16_t)delta);
    } else {
        hue = (int16_t)(171 + 43 * (int16_t)(r - g) / (int16_t)delta);
    }
    if (hue < 0) {
        hue += 256;
    }
    return (uint8_t)hue;
}

static void palette_color_for_hue(const led_runtime_t *runtime, uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (runtime == NULL || r == NULL || g == NULL || b == NULL) {
        return;
    }

    if (runtime->effect_palette_mode == LED_PALETTE_MODE_DUO) {
        uint8_t t = hue;
        *r = lerp_u8(runtime->effect_palette_color1_r, runtime->effect_palette_color2_r, t);
        *g = lerp_u8(runtime->effect_palette_color1_g, runtime->effect_palette_color2_g, t);
        *b = lerp_u8(runtime->effect_palette_color1_b, runtime->effect_palette_color2_b, t);
        return;
    }

    if (runtime->effect_palette_mode == LED_PALETTE_MODE_TRIO) {
        if (hue < 128U) {
            uint8_t t = (uint8_t)(hue * 2U);
            *r = lerp_u8(runtime->effect_palette_color1_r, runtime->effect_palette_color2_r, t);
            *g = lerp_u8(runtime->effect_palette_color1_g, runtime->effect_palette_color2_g, t);
            *b = lerp_u8(runtime->effect_palette_color1_b, runtime->effect_palette_color2_b, t);
        } else {
            uint8_t t = (uint8_t)((hue - 128U) * 2U);
            *r = lerp_u8(runtime->effect_palette_color2_r, runtime->effect_palette_color3_r, t);
            *g = lerp_u8(runtime->effect_palette_color2_g, runtime->effect_palette_color3_g, t);
            *b = lerp_u8(runtime->effect_palette_color2_b, runtime->effect_palette_color3_b, t);
        }
        return;
    }

    *r = s_framebuffer[0];
    *g = s_framebuffer[1];
    *b = s_framebuffer[2];
}

void led_task_apply_effect_palette(led_runtime_t *runtime)
{
    if (runtime == NULL || runtime->effect_palette_mode == LED_PALETTE_MODE_RAINBOW) {
        return;
    }

    for (uint32_t i = 0; i < LED_FRAMEBUFFER_BYTES; i += 3U) {
        uint8_t src_r = s_framebuffer[i + 0U];
        uint8_t src_g = s_framebuffer[i + 1U];
        uint8_t src_b = s_framebuffer[i + 2U];
        uint8_t max = (src_r > src_g) ? ((src_r > src_b) ? src_r : src_b) : ((src_g > src_b) ? src_g : src_b);
        if (max == 0U) {
            continue;
        }

        uint8_t hue = rgb_hue_u8(src_r, src_g, src_b);
        uint8_t map_r = 0U;
        uint8_t map_g = 0U;
        uint8_t map_b = 0U;

        if (runtime->effect_palette_mode == LED_PALETTE_MODE_DUO) {
            uint8_t t = hue;
            map_r = lerp_u8(runtime->effect_palette_color1_r, runtime->effect_palette_color2_r, t);
            map_g = lerp_u8(runtime->effect_palette_color1_g, runtime->effect_palette_color2_g, t);
            map_b = lerp_u8(runtime->effect_palette_color1_b, runtime->effect_palette_color2_b, t);
        } else if (runtime->effect_palette_mode == LED_PALETTE_MODE_TRIO) {
            if (hue < 128U) {
                uint8_t t = (uint8_t)(hue * 2U);
                map_r = lerp_u8(runtime->effect_palette_color1_r, runtime->effect_palette_color2_r, t);
                map_g = lerp_u8(runtime->effect_palette_color1_g, runtime->effect_palette_color2_g, t);
                map_b = lerp_u8(runtime->effect_palette_color1_b, runtime->effect_palette_color2_b, t);
            } else {
                uint8_t t = (uint8_t)((hue - 128U) * 2U);
                map_r = lerp_u8(runtime->effect_palette_color2_r, runtime->effect_palette_color3_r, t);
                map_g = lerp_u8(runtime->effect_palette_color2_g, runtime->effect_palette_color3_g, t);
                map_b = lerp_u8(runtime->effect_palette_color2_b, runtime->effect_palette_color3_b, t);
            }
        }

        s_framebuffer[i + 0U] = scale_u8_permille(map_r, (uint16_t)max * 1000U / 255U);
        s_framebuffer[i + 1U] = scale_u8_permille(map_g, (uint16_t)max * 1000U / 255U);
        s_framebuffer[i + 2U] = scale_u8_permille(map_b, (uint16_t)max * 1000U / 255U);
    }
}

static uint32_t matrix_to_strip_index(uint32_t x, uint32_t y)
{
    if (x >= LED_MATRIX_W || y >= LED_MATRIX_H) {
        return 0;
    }

    uint32_t mx = x;
    uint32_t my = y;

#if CONFIG_ORB_LED_MATRIX_ROTATE_90_CW
    uint32_t tx = (LED_MATRIX_W - 1U) - my;
    uint32_t ty = mx;
    mx = tx;
    my = ty;
#elif CONFIG_ORB_LED_MATRIX_ROTATE_90_CCW
    uint32_t tx = my;
    uint32_t ty = (LED_MATRIX_H - 1U) - mx;
    mx = tx;
    my = ty;
#elif CONFIG_ORB_LED_MATRIX_ROTATE_180
    mx = (LED_MATRIX_W - 1U) - mx;
    my = (LED_MATRIX_H - 1U) - my;
#endif

    if (my % 2U == 0U) {
        return my * LED_MATRIX_W + mx;
    }
    return my * LED_MATRIX_W + ((LED_MATRIX_W - 1U) - mx);
}

void led_task_framebuffer_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

static void framebuffer_set_pixel_rgb(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    if (x >= LED_MATRIX_W || y >= LED_MATRIX_H) {
        return;
    }
    uint32_t idx = matrix_to_strip_index(x, y);
    uint32_t off = idx * 3U;
    s_framebuffer[off + 0U] = scale_u8(g, brightness);
    s_framebuffer[off + 1U] = scale_u8(r, brightness);
    s_framebuffer[off + 2U] = scale_u8(b, brightness);
}

static void framebuffer_scale_all(uint16_t gain_permille)
{
    for (uint32_t i = 0; i < LED_FRAMEBUFFER_BYTES; ++i) {
        s_framebuffer[i] = (uint8_t)(((uint16_t)s_framebuffer[i] * gain_permille) / 1000U);
    }
}

static uint32_t estimate_frame_current_ma(void)
{
    uint32_t total = CONFIG_ORB_LED_IDLE_CURRENT_MA;
    for (uint32_t i = 0; i < LED_FRAMEBUFFER_BYTES; i += 3U) {
        total += ((uint32_t)s_framebuffer[i + 0U] * CONFIG_ORB_LED_MAX_COLOR_CHANNEL_MA) / 255U;
        total += ((uint32_t)s_framebuffer[i + 1U] * CONFIG_ORB_LED_MAX_COLOR_CHANNEL_MA) / 255U;
        total += ((uint32_t)s_framebuffer[i + 2U] * CONFIG_ORB_LED_MAX_COLOR_CHANNEL_MA) / 255U;
    }
    return total;
}

static uint16_t audio_reactive_safe_ceiling_permille(void)
{
    uint32_t estimated = estimate_frame_current_ma();
    if (estimated >= CONFIG_ORB_LED_MAX_CURRENT_MA) {
        return 1000U;
    }
    uint32_t budget = CONFIG_ORB_LED_MAX_CURRENT_MA - estimated;
    uint32_t headroom = budget * 1000U / CONFIG_ORB_LED_MAX_CURRENT_MA;
    uint32_t max_boost = 1000U + headroom;
    if (max_boost > 1200U) {
        max_boost = 1200U;
    }
    return (uint16_t)max_boost;
}

static uint8_t zone_for_x(uint32_t x)
{
    uint32_t zone_width = LED_MATRIX_W / LED_TOUCH_ZONE_COUNT;
    if (zone_width == 0U) {
        return 0U;
    }
    uint8_t zone = (uint8_t)(x / zone_width);
    if (zone >= LED_TOUCH_ZONE_COUNT) {
        zone = LED_TOUCH_ZONE_COUNT - 1U;
    }
    return zone;
}

static void framebuffer_boost_pixel(uint32_t x, uint32_t y, uint16_t gain_permille, uint8_t lift_max)
{
    if (x >= LED_MATRIX_W || y >= LED_MATRIX_H) {
        return;
    }
    uint32_t idx = matrix_to_strip_index(x, y);
    uint32_t off = idx * 3U;
    uint8_t g = scale_u8_permille(s_framebuffer[off + 0U], gain_permille);
    uint8_t r = scale_u8_permille(s_framebuffer[off + 1U], gain_permille);
    uint8_t b = scale_u8_permille(s_framebuffer[off + 2U], gain_permille);
    g = add_sat_u8(g, (uint8_t)(lift_max / 2U));
    r = add_sat_u8(r, lift_max);
    b = add_sat_u8(b, (uint8_t)(lift_max / 3U));
    s_framebuffer[off + 0U] = g;
    s_framebuffer[off + 1U] = r;
    s_framebuffer[off + 2U] = b;
}

void led_task_effects_set_pixel_cb(uint32_t x,
                                   uint32_t y,
                                   uint8_t r,
                                   uint8_t g,
                                   uint8_t b,
                                   uint8_t brightness,
                                   void *ctx)
{
    (void)ctx;
    framebuffer_set_pixel_rgb(x, y, r, g, b, brightness);
}

void led_task_effects_fill_cb(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness, void *ctx)
{
    (void)ctx;
    for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
        for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
            framebuffer_set_pixel_rgb(x, y, r, g, b, brightness);
        }
    }
}

void led_task_effects_clear_cb(void *ctx)
{
    (void)ctx;
    led_task_framebuffer_clear();
}

static uint8_t zone_overlay_intensity(led_touch_zone_overlay_t *zone, uint32_t now_ms)
{
    if (zone == NULL) {
        return 0U;
    }
    if (zone->pressed) {
        return 255U;
    }
    if (!zone->fade_active) {
        return 0U;
    }

    uint32_t elapsed = now_ms - zone->fade_start_ms;
    uint32_t fade_ms = CONFIG_ORB_LED_TOUCH_OVERLAY_FADE_MS;
    if (fade_ms == 0U || elapsed >= fade_ms) {
        zone->fade_active = false;
        return 0U;
    }

    uint32_t remain = fade_ms - elapsed;
    return (uint8_t)((remain * 255U) / fade_ms);
}

void led_task_apply_touch_overlay(led_runtime_t *runtime, uint32_t now_ms)
{
    if (runtime == NULL) {
        return;
    }

    for (uint32_t zone_id = 0; zone_id < LED_TOUCH_ZONE_COUNT; ++zone_id) {
        uint8_t intensity = zone_overlay_intensity(&runtime->touch_zone[zone_id], now_ms);
        if (intensity == 0U) {
            continue;
        }

        uint8_t hue = (uint8_t)(zone_id * (256U / LED_TOUCH_ZONE_COUNT));
        uint8_t target_r = 0U;
        uint8_t target_g = 0U;
        uint8_t target_b = 0U;
        palette_color_for_hue(runtime, hue, &target_r, &target_g, &target_b);

        uint32_t x_begin = (zone_id * LED_MATRIX_W) / LED_TOUCH_ZONE_COUNT;
        uint32_t x_end = ((zone_id + 1U) * LED_MATRIX_W) / LED_TOUCH_ZONE_COUNT;
        if (x_end <= x_begin) {
            x_end = x_begin + 1U;
        }

        for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
            for (uint32_t x = x_begin; x < x_end; ++x) {
                uint32_t idx = matrix_to_strip_index(x, y);
                uint32_t off = idx * 3U;
                uint8_t cur_g = s_framebuffer[off + 0U];
                uint8_t cur_r = s_framebuffer[off + 1U];
                uint8_t cur_b = s_framebuffer[off + 2U];
                uint8_t new_r = lerp_u8(cur_r, target_r, intensity);
                uint8_t new_g = lerp_u8(cur_g, target_g, intensity);
                uint8_t new_b = lerp_u8(cur_b, target_b, intensity);
                s_framebuffer[off + 0U] = new_g;
                s_framebuffer[off + 1U] = new_r;
                s_framebuffer[off + 2U] = new_b;
            }
        }
    }
}

void led_task_apply_audio_reactive_gain(led_runtime_t *runtime, uint32_t now_ms)
{
#if !CONFIG_ORB_LED_AUDIO_REACTIVE_ENABLE
    (void)runtime;
    (void)now_ms;
    return;
#else
    if (runtime == NULL || !runtime->audio_reactive_active) {
        return;
    }
    if ((now_ms - runtime->audio_reactive_last_update_ms) > CONFIG_ORB_LED_AUDIO_REACTIVE_TIMEOUT_MS) {
        runtime->audio_reactive_active = false;
        return;
    }

    uint32_t level = runtime->audio_reactive_level;
    if (level > 255U) {
        level = 255U;
    }

    uint16_t min_gain = (uint16_t)CONFIG_ORB_LED_AUDIO_REACTIVE_MIN_BRIGHTNESS_PERCENT * 10U;
    uint16_t max_gain_cfg = (uint16_t)CONFIG_ORB_LED_AUDIO_REACTIVE_MAX_BRIGHTNESS_PERCENT * 10U;
    if (max_gain_cfg < min_gain) {
        max_gain_cfg = min_gain;
    }

    uint32_t span = (uint32_t)(max_gain_cfg - min_gain);
    uint16_t dynamic_gain = (uint16_t)(min_gain + (span * level) / 255U);
    uint16_t safe_ceiling = audio_reactive_safe_ceiling_permille();
    uint16_t gain = (dynamic_gain < safe_ceiling) ? dynamic_gain : safe_ceiling;
    framebuffer_scale_all(gain);

    uint8_t lift_base = (uint8_t)((runtime->audio_reactive_level * 96U) / 255U);
    uint16_t zone_boost_gain = (uint16_t)(1000U + ((uint32_t)runtime->audio_reactive_level * 800U) / 255U);
    if (zone_boost_gain > safe_ceiling) {
        zone_boost_gain = safe_ceiling;
    }

    for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
        for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
            uint8_t zone = zone_for_x(x);
            if (!runtime->touch_zone[zone].pressed) {
                continue;
            }
            uint8_t lift = (uint8_t)(lift_base + (uint8_t)((y * 32U) / (LED_MATRIX_H ? LED_MATRIX_H : 1U)));
            framebuffer_boost_pixel(x, y, zone_boost_gain, lift);
        }
    }

    if ((now_ms - s_last_limit_log_ms) >= 1500U) {
        uint32_t estimated = estimate_frame_current_ma();
        ESP_LOGI(TAG,
                 "audio reactive level=%u gain=%u.%u%% est=%" PRIu32 "mA ceiling=%u.%u%%",
                 (unsigned)runtime->audio_reactive_level,
                 (unsigned)(gain / 10U),
                 (unsigned)(gain % 10U),
                 estimated,
                 (unsigned)(safe_ceiling / 10U),
                 (unsigned)(safe_ceiling % 10U));
    }
#endif
}

void led_task_apply_scene_transition_blend(led_runtime_t *runtime, uint32_t now_ms)
{
    if (runtime == NULL || !runtime->scene_transition_active) {
        return;
    }

    uint32_t elapsed = now_ms - runtime->scene_transition_start_ms;
    uint32_t dur = runtime->scene_transition_duration_ms;
    if (dur == 0U || elapsed >= dur) {
        runtime->scene_transition_active = false;
        runtime->scene_transition_duration_ms = 0U;
        return;
    }

    uint8_t t = (uint8_t)((elapsed * 255U) / dur);
    for (uint32_t i = 0; i < LED_FRAMEBUFFER_BYTES; ++i) {
        uint8_t from = runtime->scene_transition_from_fb[i];
        uint8_t to = s_framebuffer[i];
        s_framebuffer[i] = lerp_u8(from, to, t);
    }
}
