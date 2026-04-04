#include "led_task.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "app_tasking.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_effects.h"
#include "led_output_ws2812.h"
#include "led_power_limit.h"
#include "led_scene.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_LED;
static TaskHandle_t s_led_task_handle;
static volatile bool s_stop_requested;
static uint32_t s_last_limit_log_ms;

#ifndef CONFIG_ORB_LED_AUDIO_REACTIVE_ENABLE
#define CONFIG_ORB_LED_AUDIO_REACTIVE_ENABLE 1
#endif
#ifndef CONFIG_ORB_LED_AUDIO_REACTIVE_MIN_BRIGHTNESS_PERCENT
#define CONFIG_ORB_LED_AUDIO_REACTIVE_MIN_BRIGHTNESS_PERCENT 30
#endif
#ifndef CONFIG_ORB_LED_AUDIO_REACTIVE_MAX_BRIGHTNESS_PERCENT
#define CONFIG_ORB_LED_AUDIO_REACTIVE_MAX_BRIGHTNESS_PERCENT 240
#endif
#ifndef CONFIG_ORB_LED_AUDIO_REACTIVE_TIMEOUT_MS
#define CONFIG_ORB_LED_AUDIO_REACTIVE_TIMEOUT_MS 260
#endif
#ifndef CONFIG_ORB_LED_STOP_TIMEOUT_MS
#define CONFIG_ORB_LED_STOP_TIMEOUT_MS 300
#endif
#ifndef CONFIG_ORB_LED_SCENE_CROSSFADE_MS
#define CONFIG_ORB_LED_SCENE_CROSSFADE_MS 700
#endif

#define LED_MATRIX_W ((uint32_t)CONFIG_ORB_LED_MATRIX_WIDTH)
#define LED_MATRIX_H ((uint32_t)CONFIG_ORB_LED_MATRIX_HEIGHT)
#define LED_PIXEL_COUNT (LED_MATRIX_W * LED_MATRIX_H)
#define LED_FRAMEBUFFER_BYTES (LED_PIXEL_COUNT * 3U) /* GRB */
#define LED_TOUCH_ZONE_COUNT 4U

#if (CONFIG_ORB_LED_MATRIX_ROTATE_90_CW || CONFIG_ORB_LED_MATRIX_ROTATE_90_CCW) && \
    (CONFIG_ORB_LED_MATRIX_WIDTH != CONFIG_ORB_LED_MATRIX_HEIGHT)
#error "90-degree LED matrix rotation currently requires square matrix geometry"
#endif

typedef struct {
    bool pressed;
    bool fade_active;
    uint32_t fade_start_ms;
} led_touch_zone_overlay_t;

typedef struct {
    uint8_t brightness;
    uint32_t scene_id;
    uint32_t scene_started_ms;
    uint32_t scene_duration_ms;
    uint32_t aura_transition_start_ms;
    uint32_t aura_transition_duration_ms;
    uint8_t aura_level_start;
    uint8_t aura_level_target;
    bool audio_reactive_active;
    uint8_t audio_reactive_level;
    uint32_t audio_reactive_last_update_ms;
    bool scene_transition_active;
    uint32_t scene_transition_start_ms;
    uint32_t scene_transition_duration_ms;
    uint8_t scene_transition_from_fb[LED_FRAMEBUFFER_BYTES];
    led_touch_zone_overlay_t touch_zone[LED_TOUCH_ZONE_COUNT];
    led_effects_state_t effects;
} led_runtime_t;

static led_runtime_t s_runtime;
static uint8_t s_framebuffer[LED_FRAMEBUFFER_BYTES];

static uint32_t tick_to_ms(TickType_t tick)
{
    return (uint32_t)(tick * portTICK_PERIOD_MS);
}

static TickType_t frame_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_LED_FRAME_INTERVAL_MS);
    return ticks > 0 ? ticks : 1;
}

static led_scene_id_t startup_scene_id(void)
{
#if CONFIG_ORB_DEFAULT_MODE_HYBRID_AI
    return LED_SCENE_HYBRID_IDLE_SLOW_BREATHE;
#elif CONFIG_ORB_DEFAULT_MODE_INSTALLATION_SLAVE
    return LED_SCENE_COLOR_WAVE;
#else
    return LED_SCENE_FIRE2012;
#endif
}

static uint8_t scale_u8(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * (uint16_t)brightness) / 255U);
}

static uint8_t scale_u8_permille(uint8_t value, uint16_t gain_permille)
{
    uint32_t scaled = ((uint32_t)value * (uint32_t)gain_permille) / 1000U;
    return (scaled > 255U) ? 255U : (uint8_t)scaled;
}

static uint8_t add_sat_u8(uint8_t value, uint8_t add)
{
    uint16_t sum = (uint16_t)value + (uint16_t)add;
    return (sum > 255U) ? 255U : (uint8_t)sum;
}

static uint32_t matrix_to_strip_index(uint32_t x, uint32_t y)
{
    uint32_t rx = x;
    uint32_t ry = y;

#if CONFIG_ORB_LED_MATRIX_ROTATE_90_CW
    rx = y;
    ry = (LED_MATRIX_H - 1U) - x;
#elif CONFIG_ORB_LED_MATRIX_ROTATE_180
    rx = (LED_MATRIX_W - 1U) - x;
    ry = (LED_MATRIX_H - 1U) - y;
#elif CONFIG_ORB_LED_MATRIX_ROTATE_90_CCW
    rx = (LED_MATRIX_W - 1U) - y;
    ry = x;
#endif

    x = rx;
    y = ry;

#if CONFIG_ORB_LED_MATRIX_FLIP_X
    x = (LED_MATRIX_W - 1U) - x;
#endif
#if CONFIG_ORB_LED_MATRIX_FLIP_Y
    y = (LED_MATRIX_H - 1U) - y;
#endif

#if CONFIG_ORB_LED_MATRIX_SERPENTINE
    if ((y & 1U) != 0U) {
        x = (LED_MATRIX_W - 1U) - x;
    }
#endif

    return y * LED_MATRIX_W + x;
}

static void framebuffer_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

static void framebuffer_set_pixel_rgb(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    if (x >= LED_MATRIX_W || y >= LED_MATRIX_H) {
        return;
    }

    uint32_t index = matrix_to_strip_index(x, y);
    if (index >= LED_PIXEL_COUNT) {
        return;
    }

    uint32_t base = index * 3U;
    s_framebuffer[base + 0U] = scale_u8(g, brightness); /* G */
    s_framebuffer[base + 1U] = scale_u8(r, brightness); /* R */
    s_framebuffer[base + 2U] = scale_u8(b, brightness); /* B */
}

static uint8_t max3_u8(uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t m = (a > b) ? a : b;
    return (m > c) ? m : c;
}

static void framebuffer_scale_pixel(uint32_t x, uint32_t y, uint16_t gain_permille)
{
    if (x >= LED_MATRIX_W || y >= LED_MATRIX_H) {
        return;
    }
    uint32_t index = matrix_to_strip_index(x, y);
    if (index >= LED_PIXEL_COUNT) {
        return;
    }

    uint32_t base = index * 3U;
    s_framebuffer[base + 0U] = scale_u8_permille(s_framebuffer[base + 0U], gain_permille); /* G */
    s_framebuffer[base + 1U] = scale_u8_permille(s_framebuffer[base + 1U], gain_permille); /* R */
    s_framebuffer[base + 2U] = scale_u8_permille(s_framebuffer[base + 2U], gain_permille); /* B */
}

#if CONFIG_ORB_LED_AUDIO_REACTIVE_ENABLE
static void framebuffer_scale_all(uint16_t gain_permille)
{
    for (uint32_t i = 0; i < LED_FRAMEBUFFER_BYTES; ++i) {
        s_framebuffer[i] = scale_u8_permille(s_framebuffer[i], gain_permille);
    }
}

static uint32_t estimate_frame_current_ma(void)
{
#if !CONFIG_ORB_LED_POWER_LIMIT_ENABLE
    return 0U;
#else
    uint32_t channel_sum = 0U;
    for (uint32_t i = 0; i < LED_FRAMEBUFFER_BYTES; ++i) {
        channel_sum += s_framebuffer[i];
    }

    uint32_t dynamic_ma = (channel_sum * (uint32_t)CONFIG_ORB_LED_MAX_COLOR_CHANNEL_MA) / 255U;
    uint32_t idle_ma = LED_PIXEL_COUNT * (uint32_t)CONFIG_ORB_LED_IDLE_CURRENT_MA;
    return dynamic_ma + idle_ma;
#endif
}

static uint16_t audio_reactive_safe_ceiling_permille(void)
{
#if !CONFIG_ORB_LED_POWER_LIMIT_ENABLE
    return 3000U;
#else
    uint32_t current_ma = estimate_frame_current_ma();
    if (current_ma == 0U) {
        return 3000U;
    }
    uint32_t limit_ma = (uint32_t)CONFIG_ORB_LED_MAX_CURRENT_MA;
    uint32_t ceiling = (limit_ma * 1000U) / current_ma;
    if (ceiling == 0U) {
        ceiling = 1U;
    }
    if (ceiling > 3000U) {
        ceiling = 3000U;
    }
    return (uint16_t)ceiling;
#endif
}
#endif

static uint8_t zone_for_x(uint32_t x)
{
    uint32_t zone = (x * LED_TOUCH_ZONE_COUNT) / LED_MATRIX_W;
    if (zone >= LED_TOUCH_ZONE_COUNT) {
        zone = LED_TOUCH_ZONE_COUNT - 1U;
    }
    return (uint8_t)zone;
}

static void framebuffer_boost_pixel(uint32_t x, uint32_t y, uint16_t gain_permille, uint8_t lift_max)
{
    if (x >= LED_MATRIX_W || y >= LED_MATRIX_H || gain_permille <= 1000U) {
        return;
    }

    uint32_t index = matrix_to_strip_index(x, y);
    if (index >= LED_PIXEL_COUNT) {
        return;
    }

    uint32_t base = index * 3U;
    uint8_t g0 = s_framebuffer[base + 0U];
    uint8_t r0 = s_framebuffer[base + 1U];
    uint8_t b0 = s_framebuffer[base + 2U];
    uint8_t max_ch = max3_u8(g0, r0, b0);
    uint8_t g = g0;
    uint8_t r = r0;
    uint8_t b = b0;

    if (max_ch > 0U) {
        /* Limit gain to avoid channel clipping, preserving hue/saturation. */
        uint16_t safe_gain_permille = (uint16_t)(255000U / (uint32_t)max_ch);
        uint16_t effective_gain = gain_permille;
        if (effective_gain > safe_gain_permille) {
            effective_gain = safe_gain_permille;
        }

        g = scale_u8_permille(g0, effective_gain);
        r = scale_u8_permille(r0, effective_gain);
        b = scale_u8_permille(b0, effective_gain);
    }

    if (lift_max > 0U) {
        uint8_t max_after = max3_u8(g, r, b);
        uint8_t lift = lift_max;
        if (max_after > 0U) {
            g = add_sat_u8(g, (uint8_t)(((uint16_t)lift * g) / max_after));
            r = add_sat_u8(r, (uint8_t)(((uint16_t)lift * r) / max_after));
            b = add_sat_u8(b, (uint8_t)(((uint16_t)lift * b) / max_after));
        }
    }

    s_framebuffer[base + 0U] = g; /* G */
    s_framebuffer[base + 1U] = r; /* R */
    s_framebuffer[base + 2U] = b; /* B */
}

static void effects_set_pixel_cb(uint32_t x,
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

static void effects_fill_cb(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness, void *ctx)
{
    (void)ctx;
    for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
        for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
            framebuffer_set_pixel_rgb(x, y, r, g, b, brightness);
        }
    }
}

static void effects_clear_cb(void *ctx)
{
    (void)ctx;
    framebuffer_clear();
}

static uint8_t zone_overlay_intensity(led_touch_zone_overlay_t *zone, uint32_t now_ms)
{
    if (zone->pressed) {
        return (uint8_t)CONFIG_ORB_LED_TOUCH_OVERLAY_INTENSITY;
    }

    if (!zone->fade_active) {
        return 0U;
    }

    uint32_t fade_ms = (uint32_t)CONFIG_ORB_LED_TOUCH_OVERLAY_FADE_MS;
    if (fade_ms == 0U) {
        zone->fade_active = false;
        return 0U;
    }

    uint32_t elapsed = now_ms - zone->fade_start_ms;
    if (elapsed >= fade_ms) {
        zone->fade_active = false;
        return 0U;
    }

    uint32_t remain = fade_ms - elapsed;
    return (uint8_t)(((uint32_t)CONFIG_ORB_LED_TOUCH_OVERLAY_INTENSITY * remain) / fade_ms);
}

static void apply_touch_overlay(led_runtime_t *runtime, uint32_t now_ms)
{
#if !CONFIG_ORB_LED_TOUCH_OVERLAY_ENABLE
    (void)runtime;
    (void)now_ms;
    return;
#else
    uint8_t active_mask = 0U;
    for (uint8_t zone_id = 0; zone_id < LED_TOUCH_ZONE_COUNT; ++zone_id) {
        if (runtime->touch_zone[zone_id].pressed) {
            active_mask |= (uint8_t)(1U << zone_id);
        }
    }

#if CONFIG_ORB_LED_TOUCH_OVERLAY_BACKGROUND_DIM_PERCENT < 100
    if (active_mask != 0U) {
        const uint16_t bg_gain = (uint16_t)(CONFIG_ORB_LED_TOUCH_OVERLAY_BACKGROUND_DIM_PERCENT * 10U);
        for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
            for (uint32_t x = 0; x < LED_MATRIX_W; ++x) {
                uint8_t zone = zone_for_x(x);
                if ((active_mask & (uint8_t)(1U << zone)) != 0U) {
                    continue;
                }
                framebuffer_scale_pixel(x, y, bg_gain);
            }
        }
    }
#endif

    for (uint8_t zone_id = 0; zone_id < LED_TOUCH_ZONE_COUNT; ++zone_id) {
        uint8_t intensity = zone_overlay_intensity(&runtime->touch_zone[zone_id], now_ms);
        if (intensity == 0U) {
            continue;
        }
        uint16_t extra_permille =
            (uint16_t)(((uint32_t)intensity * (uint32_t)CONFIG_ORB_LED_TOUCH_OVERLAY_MAX_BOOST_PERCENT * 10U) / 255U);
        uint16_t gain_permille = (uint16_t)(1000U + extra_permille);
        uint8_t lift = (uint8_t)(((uint32_t)intensity * (uint32_t)CONFIG_ORB_LED_TOUCH_OVERLAY_LIFT_MAX) / 255U);

        uint32_t x_start = ((uint32_t)zone_id * LED_MATRIX_W) / LED_TOUCH_ZONE_COUNT;
        uint32_t x_end = (((uint32_t)zone_id + 1U) * LED_MATRIX_W) / LED_TOUCH_ZONE_COUNT;
        if (x_end <= x_start) {
            x_end = x_start + 1U;
        }
        if (x_end > LED_MATRIX_W) {
            x_end = LED_MATRIX_W;
        }

        for (uint32_t y = 0; y < LED_MATRIX_H; ++y) {
            for (uint32_t x = x_start; x < x_end; ++x) {
                framebuffer_boost_pixel(x, y, gain_permille, lift);
            }
        }
    }
#endif
}

static void apply_audio_reactive_gain(led_runtime_t *runtime, uint32_t now_ms)
{
#if !CONFIG_ORB_LED_AUDIO_REACTIVE_ENABLE
    (void)runtime;
    (void)now_ms;
    return;
#else
    if (!runtime->audio_reactive_active) {
        return;
    }

    uint32_t stale_ms = now_ms - runtime->audio_reactive_last_update_ms;
    if (stale_ms > (uint32_t)CONFIG_ORB_LED_AUDIO_REACTIVE_TIMEOUT_MS) {
        runtime->audio_reactive_active = false;
        runtime->audio_reactive_level = 0U;
        return;
    }

    uint32_t min_pct = (uint32_t)CONFIG_ORB_LED_AUDIO_REACTIVE_MIN_BRIGHTNESS_PERCENT;
    uint32_t max_pct = (uint32_t)CONFIG_ORB_LED_AUDIO_REACTIVE_MAX_BRIGHTNESS_PERCENT;
    uint32_t level = runtime->audio_reactive_level;
    const uint32_t noise_gate = 3U;

    if (min_pct > max_pct) {
        uint32_t tmp = max_pct;
        max_pct = min_pct;
        min_pct = tmp;
    }

    uint16_t safe_ceiling = audio_reactive_safe_ceiling_permille();
    uint32_t min_permille = min_pct * 10U;
    uint32_t max_permille = max_pct * 10U;
    if (min_permille > (uint32_t)safe_ceiling) {
        min_permille = (uint32_t)safe_ceiling;
    }
    if (max_permille > (uint32_t)safe_ceiling) {
        max_permille = (uint32_t)safe_ceiling;
    }
    if (max_permille < min_permille) {
        max_permille = min_permille;
    }

    uint32_t lvl = 0U;
    if (level > noise_gate) {
        lvl = ((level - noise_gate) * 255U) / (255U - noise_gate);
    }

    /* Direct two-sided mapping:
     * low audio -> dim frame towards min_permille,
     * high audio -> boost frame towards max_permille.
     */
    uint32_t gain_permille_u32 = min_permille + ((max_permille - min_permille) * lvl) / 255U;

    uint16_t gain_permille = (gain_permille_u32 > 3000U) ? 3000U : (uint16_t)gain_permille_u32;
    framebuffer_scale_all(gain_permille);
#endif
}

static void begin_scene_transition(led_runtime_t *runtime, uint32_t now_ms)
{
    if (runtime == NULL) {
        return;
    }
    runtime->scene_transition_active = true;
    runtime->scene_transition_start_ms = now_ms;
    runtime->scene_transition_duration_ms = (uint32_t)CONFIG_ORB_LED_SCENE_CROSSFADE_MS;
    if (runtime->scene_transition_duration_ms == 0U) {
        runtime->scene_transition_duration_ms = 1U;
    }
    memcpy(runtime->scene_transition_from_fb, s_framebuffer, sizeof(runtime->scene_transition_from_fb));
}

static void set_scene_runtime(led_runtime_t *runtime, uint32_t scene_id, uint32_t duration_ms, uint32_t now_ms, bool with_transition)
{
    if (runtime == NULL) {
        return;
    }

    bool scene_changed = (runtime->scene_id != scene_id);
    if (with_transition && scene_changed && runtime->scene_id != 0U) {
        begin_scene_transition(runtime, now_ms);
    } else if (scene_changed) {
        runtime->scene_transition_active = false;
        runtime->scene_transition_duration_ms = 0U;
    }

    if (scene_changed) {
        led_effects_reset_state(&runtime->effects, now_ms);
    }

    runtime->scene_id = scene_id;
    runtime->scene_started_ms = now_ms;
    runtime->scene_duration_ms = duration_ms;
}

static void apply_scene_transition_blend(led_runtime_t *runtime, uint32_t now_ms)
{
    if (runtime == NULL || !runtime->scene_transition_active) {
        return;
    }

    uint32_t elapsed = now_ms - runtime->scene_transition_start_ms;
    uint32_t duration = runtime->scene_transition_duration_ms;
    if (duration == 0U || elapsed >= duration) {
        runtime->scene_transition_active = false;
        runtime->scene_transition_duration_ms = 0U;
        return;
    }

    uint32_t old_w = duration - elapsed;
    uint32_t new_w = elapsed;
    for (uint32_t i = 0; i < LED_FRAMEBUFFER_BYTES; ++i) {
        uint32_t old_v = runtime->scene_transition_from_fb[i];
        uint32_t new_v = s_framebuffer[i];
        s_framebuffer[i] = (uint8_t)((old_v * old_w + new_v * new_w) / duration);
    }
}

static void maybe_apply_scene_timeout(led_runtime_t *runtime, uint32_t now_ms)
{
    if (runtime->scene_duration_ms == 0U || runtime->scene_id == LED_SCENE_IDLE_BREATHE) {
        return;
    }

    if ((now_ms - runtime->scene_started_ms) >= runtime->scene_duration_ms) {
        set_scene_runtime(runtime, LED_SCENE_IDLE_BREATHE, 0U, now_ms, true);
    }
}

static void maybe_update_aura_transition(led_runtime_t *runtime, uint32_t now_ms)
{
    if (runtime->aura_transition_duration_ms == 0U) {
        runtime->effects.aura_level = runtime->aura_level_target;
        return;
    }

    uint32_t elapsed = now_ms - runtime->aura_transition_start_ms;
    if (elapsed >= runtime->aura_transition_duration_ms) {
        runtime->effects.aura_level = runtime->aura_level_target;
        runtime->aura_transition_duration_ms = 0U;
        return;
    }

    int32_t delta = (int32_t)runtime->aura_level_target - (int32_t)runtime->aura_level_start;
    int32_t level = (int32_t)runtime->aura_level_start +
                    (int32_t)(((int64_t)delta * (int64_t)elapsed) / (int64_t)runtime->aura_transition_duration_ms);
    if (level < 0) {
        level = 0;
    } else if (level > 255) {
        level = 255;
    }
    runtime->effects.aura_level = (uint8_t)level;
}

static void handle_command(led_runtime_t *runtime, const led_command_t *cmd)
{
    if (runtime == NULL || cmd == NULL) {
        return;
    }

    uint32_t now_ms = tick_to_ms(xTaskGetTickCount());
    switch (cmd->id) {
    case LED_CMD_PLAY_SCENE: {
        uint32_t new_scene = cmd->payload.play_scene.scene_id;
        set_scene_runtime(runtime, new_scene, cmd->payload.play_scene.duration_ms, now_ms, true);
        ESP_LOGI(TAG,
                 "PLAY_SCENE id=%" PRIu32 " (%s) duration=%" PRIu32 "ms",
                 runtime->scene_id,
                 led_scene_name(runtime->scene_id),
                 runtime->scene_duration_ms);
        break;
    }
    case LED_CMD_STOP:
        runtime->scene_id = 0U;
        runtime->scene_duration_ms = 0U;
        runtime->aura_transition_duration_ms = 0U;
        runtime->aura_level_start = 0U;
        runtime->aura_level_target = 0U;
        runtime->audio_reactive_active = false;
        runtime->audio_reactive_level = 0U;
        runtime->audio_reactive_last_update_ms = 0U;
        runtime->scene_transition_active = false;
        runtime->scene_transition_duration_ms = 0U;
        memset(runtime->touch_zone, 0, sizeof(runtime->touch_zone));
        led_effects_reset_state(&runtime->effects, now_ms);
        framebuffer_clear();
        (void)led_output_ws2812_clear(CONFIG_ORB_LED_TX_TIMEOUT_MS);
        ESP_LOGI(TAG, "STOP");
        break;
    case LED_CMD_SET_BRIGHTNESS:
        runtime->brightness = cmd->payload.set_brightness.brightness;
        ESP_LOGI(TAG, "SET_BRIGHTNESS %u", (unsigned)runtime->brightness);
        break;
    case LED_CMD_TOUCH_ZONE_SET: {
        uint8_t zone_id = cmd->payload.touch_zone.zone_id;
        bool pressed = (cmd->payload.touch_zone.pressed != 0U);
        if (zone_id >= LED_TOUCH_ZONE_COUNT) {
            ESP_LOGW(TAG, "TOUCH_ZONE_SET invalid zone=%u", zone_id);
            break;
        }
        if (pressed) {
            runtime->touch_zone[zone_id].pressed = true;
            runtime->touch_zone[zone_id].fade_active = false;
        } else {
            runtime->touch_zone[zone_id].pressed = false;
            runtime->touch_zone[zone_id].fade_active = true;
            runtime->touch_zone[zone_id].fade_start_ms = now_ms;
        }
        break;
    }
    case LED_CMD_TOUCH_OVERLAY_CLEAR:
        memset(runtime->touch_zone, 0, sizeof(runtime->touch_zone));
        break;
    case LED_CMD_SET_AURA_COLOR:
        runtime->effects.aura_r = cmd->payload.aura_color.r;
        runtime->effects.aura_g = cmd->payload.aura_color.g;
        runtime->effects.aura_b = cmd->payload.aura_color.b;
        runtime->aura_transition_start_ms = now_ms;
        runtime->aura_transition_duration_ms = cmd->payload.aura_color.ramp_ms;
        runtime->aura_level_start = runtime->effects.aura_level;
        runtime->aura_level_target = 255U;
        if (runtime->aura_transition_duration_ms == 0U) {
            runtime->effects.aura_level = 255U;
        }
        ESP_LOGI(TAG,
                 "AURA_COLOR rgb=(%u,%u,%u) ramp=%" PRIu32 "ms",
                 (unsigned)runtime->effects.aura_r,
                 (unsigned)runtime->effects.aura_g,
                 (unsigned)runtime->effects.aura_b,
                 runtime->aura_transition_duration_ms);
        break;
    case LED_CMD_AURA_FADE_OUT:
        runtime->aura_transition_start_ms = now_ms;
        runtime->aura_transition_duration_ms = cmd->payload.aura_fade_out.duration_ms;
        runtime->aura_level_start = runtime->effects.aura_level;
        runtime->aura_level_target = 0U;
        if (runtime->aura_transition_duration_ms == 0U) {
            runtime->effects.aura_level = 0U;
        }
        ESP_LOGI(TAG, "AURA_FADE_OUT duration=%" PRIu32 "ms", runtime->aura_transition_duration_ms);
        break;
    case LED_CMD_SET_AUDIO_REACTIVE_LEVEL:
        runtime->audio_reactive_level = cmd->payload.audio_level.level;
        runtime->audio_reactive_last_update_ms = now_ms;
        runtime->audio_reactive_active = true;
        break;
    case LED_CMD_NONE:
    default:
        ESP_LOGW(TAG, "unknown command id=%d", (int)cmd->id);
        break;
    }
}

static void led_task_entry(void *arg)
{
    (void)arg;

    QueueHandle_t queue = app_tasking_get_led_cmd_queue();
    if (queue == NULL) {
        ESP_LOGE(TAG, "led_cmd_queue is not initialized");
        s_led_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.brightness = CONFIG_ORB_LED_DEFAULT_BRIGHTNESS;
    s_runtime.scene_id = startup_scene_id();
    s_runtime.scene_started_ms = tick_to_ms(xTaskGetTickCount());
    s_runtime.scene_transition_active = false;
    s_runtime.scene_transition_duration_ms = 0U;
    led_effects_reset_state(&s_runtime.effects, s_runtime.scene_started_ms);
    s_runtime.aura_transition_start_ms = s_runtime.scene_started_ms;
    s_runtime.aura_transition_duration_ms = 0U;
    s_runtime.aura_level_start = 0U;
    s_runtime.aura_level_target = 0U;
    s_runtime.audio_reactive_active = false;
    s_runtime.audio_reactive_level = 0U;
    s_runtime.audio_reactive_last_update_ms = 0U;

    ESP_LOGI(TAG,
             "led_task started matrix=%ux%u fps=%u",
             (unsigned)LED_MATRIX_W,
             (unsigned)LED_MATRIX_H,
             (unsigned)(1000U / CONFIG_ORB_LED_FRAME_INTERVAL_MS));
    ESP_LOGI(TAG,
             "led defaults brightness=%u limiter=%s cap=%umA channel=%umA idle=%umA",
             (unsigned)s_runtime.brightness,
#if CONFIG_ORB_LED_POWER_LIMIT_ENABLE
             "on",
             (unsigned)CONFIG_ORB_LED_MAX_CURRENT_MA,
             (unsigned)CONFIG_ORB_LED_MAX_COLOR_CHANNEL_MA,
             (unsigned)CONFIG_ORB_LED_IDLE_CURRENT_MA);
#else
             "off",
             0U,
             0U,
             0U);
#endif

    TickType_t last_wake = xTaskGetTickCount();
    TickType_t frame_period = frame_ticks();
    while (!s_stop_requested) {
        led_command_t cmd = { 0 };
        while (xQueueReceive(queue, &cmd, 0) == pdTRUE) {
            handle_command(&s_runtime, &cmd);
        }

        uint32_t now_ms = tick_to_ms(xTaskGetTickCount());
        maybe_apply_scene_timeout(&s_runtime, now_ms);
        maybe_update_aura_transition(&s_runtime, now_ms);
        uint32_t scene_elapsed_ms = now_ms - s_runtime.scene_started_ms;
        led_effects_render_scene(s_runtime.scene_id,
                                 &s_runtime.effects,
                                 now_ms,
                                 scene_elapsed_ms,
                                 s_runtime.brightness,
                                 effects_set_pixel_cb,
                                 effects_fill_cb,
                                 effects_clear_cb,
                                 NULL);
        apply_touch_overlay(&s_runtime, now_ms);
        apply_audio_reactive_gain(&s_runtime, now_ms);
        apply_scene_transition_blend(&s_runtime, now_ms);

        if (s_runtime.scene_id != 0U) {
            led_power_limit_result_t limit_result = { 0 };
            led_power_limit_apply_grb(s_framebuffer, sizeof(s_framebuffer), &limit_result);
            if (limit_result.limited && (now_ms - s_last_limit_log_ms) >= 2000U) {
                s_last_limit_log_ms = now_ms;
                ESP_LOGI(TAG,
                         "power limit active est=%" PRIu32 "mA cap=%umA scale=%u.%u%%",
                         limit_result.estimated_current_ma_before,
                         (unsigned)CONFIG_ORB_LED_MAX_CURRENT_MA,
                         (unsigned)(limit_result.applied_scale_permille / 10U),
                         (unsigned)(limit_result.applied_scale_permille % 10U));
            }

            esp_err_t err = led_output_ws2812_write_grb(s_framebuffer, sizeof(s_framebuffer), CONFIG_ORB_LED_TX_TIMEOUT_MS);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "frame TX failed: %s", esp_err_to_name(err));
            }
        }

        vTaskDelayUntil(&last_wake, frame_period);
    }

    memset(&s_runtime, 0, sizeof(s_runtime));
    framebuffer_clear();
    (void)led_output_ws2812_clear(CONFIG_ORB_LED_TX_TIMEOUT_MS);
    s_led_task_handle = NULL;
    s_stop_requested = false;
    ESP_LOGI(TAG, "led_task stopped");
    vTaskDelete(NULL);
}

esp_err_t led_task_start(void)
{
    if (s_led_task_handle != NULL) {
        return ESP_OK;
    }
    s_stop_requested = false;

    BaseType_t ok = xTaskCreate(led_task_entry,
                                "led_task",
                                CONFIG_ORB_LED_TASK_STACK_SIZE,
                                NULL,
                                CONFIG_ORB_LED_TASK_PRIORITY,
                                &s_led_task_handle);
    if (ok != pdPASS) {
        s_led_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "task created prio=%d stack=%d", CONFIG_ORB_LED_TASK_PRIORITY, CONFIG_ORB_LED_TASK_STACK_SIZE);
    return ESP_OK;
}

esp_err_t led_task_stop(void)
{
    if (s_led_task_handle == NULL) {
        return ESP_OK;
    }

    s_stop_requested = true;
    QueueHandle_t queue = app_tasking_get_led_cmd_queue();
    if (queue != NULL) {
        led_command_t cmd = { .id = LED_CMD_STOP };
        (void)xQueueSend(queue, &cmd, 0);
    }

    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)CONFIG_ORB_LED_STOP_TIMEOUT_MS + 200U);
    while (s_led_task_handle != NULL) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGW(TAG, "led_task graceful stop timeout");
            TaskHandle_t handle = s_led_task_handle;
            s_led_task_handle = NULL;
            vTaskDelete(handle);
            s_stop_requested = false;
            (void)led_output_ws2812_clear(CONFIG_ORB_LED_TX_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}
