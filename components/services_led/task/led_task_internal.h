#ifndef LED_TASK_INTERNAL_H
#define LED_TASK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "app_tasking.h"
#include "freertos/FreeRTOS.h"
#include "led_effects.h"
#include "led_scene.h"
#include "sdkconfig.h"

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
#ifndef CONFIG_ORB_LED_SCENE_CROSSFADE_MS
#define CONFIG_ORB_LED_SCENE_CROSSFADE_MS 700
#endif
#ifndef CONFIG_ORB_LED_EFFECT_SPEED_DEFAULT
#define CONFIG_ORB_LED_EFFECT_SPEED_DEFAULT 170
#endif
#ifndef CONFIG_ORB_LED_EFFECT_INTENSITY_DEFAULT
#define CONFIG_ORB_LED_EFFECT_INTENSITY_DEFAULT 180
#endif
#ifndef CONFIG_ORB_LED_EFFECT_SCALE_DEFAULT
#define CONFIG_ORB_LED_EFFECT_SCALE_DEFAULT 140
#endif

#define LED_MATRIX_W ((uint32_t)CONFIG_ORB_LED_MATRIX_WIDTH)
#define LED_MATRIX_H ((uint32_t)CONFIG_ORB_LED_MATRIX_HEIGHT)
#define LED_PIXEL_COUNT (LED_MATRIX_W * LED_MATRIX_H)
#define LED_FRAMEBUFFER_BYTES (LED_PIXEL_COUNT * 3U)
#define LED_TOUCH_ZONE_COUNT 4U

#define LED_PALETTE_MODE_RAINBOW 0U
#define LED_PALETTE_MODE_DUO 1U
#define LED_PALETTE_MODE_TRIO 2U

typedef struct {
    bool pressed;
    bool fade_active;
    uint32_t fade_start_ms;
} led_touch_zone_overlay_t;

typedef struct {
    uint8_t brightness;
    uint8_t effect_speed;
    uint8_t effect_intensity;
    uint8_t effect_scale;
    uint8_t effect_palette_mode;
    uint8_t effect_palette_color1_r;
    uint8_t effect_palette_color1_g;
    uint8_t effect_palette_color1_b;
    uint8_t effect_palette_color2_r;
    uint8_t effect_palette_color2_g;
    uint8_t effect_palette_color2_b;
    uint8_t effect_palette_color3_r;
    uint8_t effect_palette_color3_g;
    uint8_t effect_palette_color3_b;
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

extern led_runtime_t s_runtime;
extern uint8_t s_framebuffer[LED_FRAMEBUFFER_BYTES];

static inline uint32_t led_task_tick_to_ms(TickType_t tick)
{
    return (uint32_t)(tick * portTICK_PERIOD_MS);
}

static inline TickType_t led_task_frame_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_LED_FRAME_INTERVAL_MS);
    return (ticks == 0) ? 1 : ticks;
}

led_scene_id_t led_task_startup_scene_id(void);
bool led_task_palette_mode_valid(uint8_t mode);

void led_task_framebuffer_clear(void);
void led_task_effects_set_pixel_cb(uint32_t x,
                                   uint32_t y,
                                   uint8_t r,
                                   uint8_t g,
                                   uint8_t b,
                                   uint8_t brightness,
                                   void *ctx);
void led_task_effects_fill_cb(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness, void *ctx);
void led_task_effects_clear_cb(void *ctx);
void led_task_apply_effect_palette(led_runtime_t *runtime);
void led_task_apply_touch_overlay(led_runtime_t *runtime, uint32_t now_ms);
void led_task_apply_audio_reactive_gain(led_runtime_t *runtime, uint32_t now_ms);

void led_task_set_scene_runtime(led_runtime_t *runtime,
                                uint32_t scene_id,
                                uint32_t duration_ms,
                                uint32_t now_ms,
                                bool with_transition);
void led_task_apply_scene_transition_blend(led_runtime_t *runtime, uint32_t now_ms);
void led_task_maybe_apply_scene_timeout(led_runtime_t *runtime, uint32_t now_ms);
void led_task_maybe_update_aura_transition(led_runtime_t *runtime, uint32_t now_ms);
void led_task_handle_command(led_runtime_t *runtime, const led_command_t *cmd);
void led_task_init_runtime_defaults(led_runtime_t *runtime, uint32_t now_ms);

#endif
