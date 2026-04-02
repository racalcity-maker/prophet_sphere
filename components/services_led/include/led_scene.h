#ifndef LED_SCENE_H
#define LED_SCENE_H

#include "orb_led_scenes.h"
#include "led_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LED_SCENE_IDLE_BREATHE ((led_scene_id_t)ORB_LED_SCENE_ID_IDLE_BREATHE)
#define LED_SCENE_TOUCH_AWAKE ((led_scene_id_t)ORB_LED_SCENE_ID_TOUCH_AWAKE)
#define LED_SCENE_ERROR_FLASH ((led_scene_id_t)ORB_LED_SCENE_ID_ERROR_FLASH)
#define LED_SCENE_FIRE2012 ((led_scene_id_t)ORB_LED_SCENE_ID_FIRE2012)
#define LED_SCENE_PLASMA ((led_scene_id_t)ORB_LED_SCENE_ID_PLASMA)
#define LED_SCENE_SPARKLE ((led_scene_id_t)ORB_LED_SCENE_ID_SPARKLE)
#define LED_SCENE_COLOR_WAVE ((led_scene_id_t)ORB_LED_SCENE_ID_COLOR_WAVE)
#define LED_SCENE_AURA_COLOR_BREATHE ((led_scene_id_t)ORB_LED_SCENE_ID_AURA_COLOR_BREATHE)
#define LED_SCENE_GRUMBLE_RED ((led_scene_id_t)ORB_LED_SCENE_ID_GRUMBLE_RED)
#define LED_SCENE_LOTTERY_IDLE ((led_scene_id_t)ORB_LED_SCENE_ID_LOTTERY_IDLE)
#define LED_SCENE_LOTTERY_HOLD_RAMP ((led_scene_id_t)ORB_LED_SCENE_ID_LOTTERY_HOLD_RAMP)
#define LED_SCENE_HYBRID_IDLE_SLOW_BREATHE ((led_scene_id_t)ORB_LED_SCENE_ID_HYBRID_IDLE_SLOW_BREATHE)
#define LED_SCENE_HYBRID_TOUCH_FAST_BREATHE ((led_scene_id_t)ORB_LED_SCENE_ID_HYBRID_TOUCH_FAST_BREATHE)
#define LED_SCENE_HYBRID_VORTEX ((led_scene_id_t)ORB_LED_SCENE_ID_HYBRID_VORTEX)
#define LED_SCENE_HYBRID_VORTEX_DIM ((led_scene_id_t)ORB_LED_SCENE_ID_HYBRID_VORTEX_DIM)

const char *led_scene_name(led_scene_id_t scene_id);

#ifdef __cplusplus
}
#endif

#endif
