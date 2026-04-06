#include "led_task_internal.h"

#include <inttypes.h>
#include <string.h>
#include "config_manager.h"
#include "esp_log.h"
#include "led_output_ws2812.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_LED;

led_scene_id_t led_task_startup_scene_id(void)
{
#if CONFIG_ORB_LED_STARTUP_SCENE_ID > 0
    if (led_scene_is_known((uint32_t)CONFIG_ORB_LED_STARTUP_SCENE_ID)) {
        return (led_scene_id_t)CONFIG_ORB_LED_STARTUP_SCENE_ID;
    }
#endif
    return LED_SCENE_IDLE_BREATHE;
}

static void begin_scene_transition(led_runtime_t *runtime, uint32_t now_ms)
{
    if (runtime == NULL) {
        return;
    }
    uint32_t fade_ms = (uint32_t)CONFIG_ORB_LED_SCENE_CROSSFADE_MS;
    if (fade_ms == 0U || runtime->scene_id == 0U) {
        runtime->scene_transition_active = false;
        runtime->scene_transition_duration_ms = 0U;
        return;
    }
    memcpy(runtime->scene_transition_from_fb, s_framebuffer, sizeof(runtime->scene_transition_from_fb));
    runtime->scene_transition_active = true;
    runtime->scene_transition_start_ms = now_ms;
    runtime->scene_transition_duration_ms = fade_ms;
}

void led_task_set_scene_runtime(led_runtime_t *runtime,
                                uint32_t scene_id,
                                uint32_t duration_ms,
                                uint32_t now_ms,
                                bool with_transition)
{
    if (runtime == NULL) {
        return;
    }
    if (!led_scene_is_known(scene_id)) {
        scene_id = LED_SCENE_IDLE_BREATHE;
    }
    if (with_transition) {
        begin_scene_transition(runtime, now_ms);
    } else {
        runtime->scene_transition_active = false;
        runtime->scene_transition_duration_ms = 0U;
    }
    runtime->scene_id = scene_id;
    runtime->scene_started_ms = now_ms;
    runtime->scene_duration_ms = duration_ms;
}

void led_task_maybe_apply_scene_timeout(led_runtime_t *runtime, uint32_t now_ms)
{
    if (runtime == NULL || runtime->scene_duration_ms == 0U || runtime->scene_id == 0U) {
        return;
    }

    if ((now_ms - runtime->scene_started_ms) >= runtime->scene_duration_ms) {
        led_task_set_scene_runtime(runtime, LED_SCENE_IDLE_BREATHE, 0U, now_ms, true);
    }
}

void led_task_maybe_update_aura_transition(led_runtime_t *runtime, uint32_t now_ms)
{
    if (runtime == NULL) {
        return;
    }

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

void led_task_handle_command(led_runtime_t *runtime, const led_command_t *cmd)
{
    if (runtime == NULL || cmd == NULL) {
        return;
    }

    uint32_t now_ms = led_task_tick_to_ms(xTaskGetTickCount());
    switch (cmd->id) {
    case LED_CMD_PLAY_SCENE: {
        uint32_t new_scene = cmd->payload.play_scene.scene_id;
        led_task_set_scene_runtime(runtime, new_scene, cmd->payload.play_scene.duration_ms, now_ms, true);
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
        led_task_framebuffer_clear();
        (void)led_output_ws2812_clear(CONFIG_ORB_LED_TX_TIMEOUT_MS);
        ESP_LOGI(TAG, "STOP");
        break;
    case LED_CMD_SET_BRIGHTNESS:
        runtime->brightness = cmd->payload.set_brightness.brightness;
        ESP_LOGI(TAG, "SET_BRIGHTNESS %u", (unsigned)runtime->brightness);
        break;
    case LED_CMD_SET_EFFECT_PARAMS:
        runtime->effect_speed = cmd->payload.set_effect_params.speed;
        runtime->effect_intensity = cmd->payload.set_effect_params.intensity;
        runtime->effect_scale = cmd->payload.set_effect_params.scale;
        ESP_LOGI(TAG,
                 "SET_EFFECT_PARAMS speed=%u intensity=%u scale=%u",
                 (unsigned)runtime->effect_speed,
                 (unsigned)runtime->effect_intensity,
                 (unsigned)runtime->effect_scale);
        break;
    case LED_CMD_SET_EFFECT_PALETTE:
        if (!led_task_palette_mode_valid(cmd->payload.set_effect_palette.mode)) {
            ESP_LOGW(TAG, "SET_EFFECT_PALETTE invalid mode=%u", (unsigned)cmd->payload.set_effect_palette.mode);
            break;
        }
        runtime->effect_palette_mode = cmd->payload.set_effect_palette.mode;
        runtime->effect_palette_color1_r = cmd->payload.set_effect_palette.c1_r;
        runtime->effect_palette_color1_g = cmd->payload.set_effect_palette.c1_g;
        runtime->effect_palette_color1_b = cmd->payload.set_effect_palette.c1_b;
        runtime->effect_palette_color2_r = cmd->payload.set_effect_palette.c2_r;
        runtime->effect_palette_color2_g = cmd->payload.set_effect_palette.c2_g;
        runtime->effect_palette_color2_b = cmd->payload.set_effect_palette.c2_b;
        runtime->effect_palette_color3_r = cmd->payload.set_effect_palette.c3_r;
        runtime->effect_palette_color3_g = cmd->payload.set_effect_palette.c3_g;
        runtime->effect_palette_color3_b = cmd->payload.set_effect_palette.c3_b;
        ESP_LOGI(TAG,
                 "SET_EFFECT_PALETTE mode=%u c1=(%u,%u,%u) c2=(%u,%u,%u) c3=(%u,%u,%u)",
                 (unsigned)runtime->effect_palette_mode,
                 (unsigned)runtime->effect_palette_color1_r,
                 (unsigned)runtime->effect_palette_color1_g,
                 (unsigned)runtime->effect_palette_color1_b,
                 (unsigned)runtime->effect_palette_color2_r,
                 (unsigned)runtime->effect_palette_color2_g,
                 (unsigned)runtime->effect_palette_color2_b,
                 (unsigned)runtime->effect_palette_color3_r,
                 (unsigned)runtime->effect_palette_color3_g,
                 (unsigned)runtime->effect_palette_color3_b);
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

void led_task_init_runtime_defaults(led_runtime_t *runtime, uint32_t now_ms)
{
    if (runtime == NULL) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->brightness = CONFIG_ORB_LED_DEFAULT_BRIGHTNESS;
    runtime->effect_speed = (uint8_t)CONFIG_ORB_LED_EFFECT_SPEED_DEFAULT;
    runtime->effect_intensity = (uint8_t)CONFIG_ORB_LED_EFFECT_INTENSITY_DEFAULT;
    runtime->effect_scale = (uint8_t)CONFIG_ORB_LED_EFFECT_SCALE_DEFAULT;
    runtime->effect_palette_mode = LED_PALETTE_MODE_RAINBOW;
    runtime->effect_palette_color1_r = 255U;
    runtime->effect_palette_color1_g = 0U;
    runtime->effect_palette_color1_b = 180U;
    runtime->effect_palette_color2_r = 0U;
    runtime->effect_palette_color2_g = 230U;
    runtime->effect_palette_color2_b = 255U;
    runtime->effect_palette_color3_r = 255U;
    runtime->effect_palette_color3_g = 190U;
    runtime->effect_palette_color3_b = 40U;

    orb_runtime_config_t cfg = { 0 };
    if (config_manager_get_snapshot(&cfg) == ESP_OK) {
        runtime->effect_speed = cfg.hybrid_effect_speed;
        runtime->effect_intensity = cfg.hybrid_effect_intensity;
        runtime->effect_scale = cfg.hybrid_effect_scale;
        if (led_task_palette_mode_valid(cfg.hybrid_effect_palette_mode)) {
            runtime->effect_palette_mode = cfg.hybrid_effect_palette_mode;
        }
        runtime->effect_palette_color1_r = cfg.hybrid_effect_color1_r;
        runtime->effect_palette_color1_g = cfg.hybrid_effect_color1_g;
        runtime->effect_palette_color1_b = cfg.hybrid_effect_color1_b;
        runtime->effect_palette_color2_r = cfg.hybrid_effect_color2_r;
        runtime->effect_palette_color2_g = cfg.hybrid_effect_color2_g;
        runtime->effect_palette_color2_b = cfg.hybrid_effect_color2_b;
        runtime->effect_palette_color3_r = cfg.hybrid_effect_color3_r;
        runtime->effect_palette_color3_g = cfg.hybrid_effect_color3_g;
        runtime->effect_palette_color3_b = cfg.hybrid_effect_color3_b;
    }

    runtime->scene_id = led_task_startup_scene_id();
    runtime->scene_started_ms = now_ms;
    runtime->scene_transition_active = false;
    runtime->scene_transition_duration_ms = 0U;
    led_effects_reset_state(&runtime->effects, runtime->scene_started_ms);
    runtime->aura_transition_start_ms = runtime->scene_started_ms;
    runtime->aura_transition_duration_ms = 0U;
    runtime->aura_level_start = 0U;
    runtime->aura_level_target = 0U;
    runtime->audio_reactive_active = false;
    runtime->audio_reactive_level = 0U;
    runtime->audio_reactive_last_update_ms = 0U;
}
