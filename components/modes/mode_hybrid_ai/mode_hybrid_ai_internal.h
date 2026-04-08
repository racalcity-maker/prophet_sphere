#ifndef MODE_HYBRID_AI_INTERNAL_H
#define MODE_HYBRID_AI_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "app_mode.h"
#include "orb_led_scenes.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS
#define CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_IN_MS 2000
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS
#define CONFIG_ORB_AUDIO_PROPHECY_BG_FADE_OUT_MS 4000
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE
#define CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE 260
#endif

#define HYBRID_SCENE_IDLE_ID ORB_LED_SCENE_ID_HYBRID_IDLE_SLOW_BREATHE
#define HYBRID_SCENE_TOUCH_ID ORB_LED_SCENE_ID_HYBRID_TOUCH_FAST_BREATHE
#define HYBRID_SCENE_VORTEX_ID ORB_LED_SCENE_ID_HYBRID_VORTEX
#define HYBRID_SCENE_VORTEX_LISTEN_ID ORB_LED_SCENE_ID_HYBRID_VORTEX_DIM
#define HYBRID_FAIL_ASSET_ID 1303U

#define HYBRID_MIC_CAPTURE_MS_DEFAULT 8000U
#define HYBRID_REMOTE_TTS_TIMEOUT_MS 90000U
#define HYBRID_REMOTE_FIRST_CHUNK_TIMEOUT_MS 10000U
#define HYBRID_REMOTE_STREAM_MAX_MS 90000U
#define HYBRID_REMOTE_INTRO_STREAM_MAX_MS 30000U
#define HYBRID_WS_TIMEOUT_TIMER_CODE 6
#define HYBRID_REMOTE_INTRO_TTS_TEXT "__ORACLE_INTRO__"
#define HYBRID_REMOTE_ANSWER_TTS_TEXT "__ORACLE_AUTO__"
#define HYBRID_REMOTE_RETRY_TTS_TEXT "__ORACLE_RETRY__"
#define HYBRID_TTS_BG_GAIN_MAX_PERMILLE 180U
#define HYBRID_BG_GAIN_LISTEN_PERMILLE 100U
#define HYBRID_BG_GAIN_SWITCH_FADE_MS 250U
#define HYBRID_INTENT_COLOR_RAMP_MS 800U

#ifndef CONFIG_ORB_HYBRID_UNKNOWN_RETRY_MAX
#define CONFIG_ORB_HYBRID_UNKNOWN_RETRY_MAX 1
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_IDLE_SCENE_ID
#define CONFIG_ORB_HYBRID_EFFECT_IDLE_SCENE_ID ORB_LED_SCENE_ID_HYBRID_IDLE_SLOW_BREATHE
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_TALK_SCENE_ID
#ifdef CONFIG_ORB_HYBRID_EFFECT_SCENE_ID
#define CONFIG_ORB_HYBRID_EFFECT_TALK_SCENE_ID CONFIG_ORB_HYBRID_EFFECT_SCENE_ID
#else
#define CONFIG_ORB_HYBRID_EFFECT_TALK_SCENE_ID ORB_LED_SCENE_ID_HYBRID_VORTEX
#endif
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_SPEED_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_SPEED_DEFAULT 170
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_INTENSITY_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_INTENSITY_DEFAULT 180
#endif
#ifndef CONFIG_ORB_HYBRID_EFFECT_SCALE_DEFAULT
#define CONFIG_ORB_HYBRID_EFFECT_SCALE_DEFAULT 140
#endif

typedef enum {
    HYBRID_FLOW_IDLE = 0,
    HYBRID_FLOW_WAIT_REMOTE_INTRO_TTS_DONE,
    HYBRID_FLOW_WAIT_MIC_DONE,
    HYBRID_FLOW_WAIT_REMOTE_RETRY_TTS_DONE,
    HYBRID_FLOW_WAIT_REMOTE_ANSWER_TTS_DONE,
    HYBRID_FLOW_WAIT_FAIL_PROMPT_DONE,
    HYBRID_FLOW_WAIT_BG_FADE_OUT,
} hybrid_flow_t;

typedef struct {
    uint32_t mic_capture_ms;
    uint32_t bg_fade_in_ms;
    uint32_t bg_fade_out_ms;
    uint16_t bg_gain_permille;
    uint8_t unknown_retry_max;
    uint32_t effect_idle_scene_id;
    uint32_t effect_talk_scene_id;
    uint8_t effect_speed;
    uint8_t effect_intensity;
    uint8_t effect_scale;
} hybrid_runtime_cfg_t;

void hybrid_policy_load_runtime_cfg(hybrid_runtime_cfg_t *runtime);
uint8_t hybrid_unknown_retry_limit(const hybrid_runtime_cfg_t *runtime);
bool hybrid_intent_is_unknown_like(uint8_t intent_id);
void hybrid_resolve_intent_color(uint8_t intent_id, uint8_t *r, uint8_t *g, uint8_t *b);

uint32_t hybrid_effect_scene_normalize(uint32_t scene_id, uint32_t fallback_scene_id);
uint32_t hybrid_effect_scene_speak(const hybrid_runtime_cfg_t *runtime);
uint32_t hybrid_effect_scene_idle(const hybrid_runtime_cfg_t *runtime);
uint32_t hybrid_effect_scene_listen(const hybrid_runtime_cfg_t *runtime);

uint32_t hybrid_scene_for_flow(hybrid_flow_t flow, const hybrid_runtime_cfg_t *runtime);
void hybrid_set_flow_and_scene(hybrid_flow_t *flow,
                               hybrid_flow_t next_flow,
                               const hybrid_runtime_cfg_t *runtime,
                               app_mode_action_t *action);

const char *hybrid_flow_name(hybrid_flow_t flow);
const char *hybrid_event_name(app_mode_event_id_t id);

bool hybrid_remote_event_is_bg_fade_done(const app_mode_event_t *event);
bool hybrid_remote_waiting_tts(hybrid_flow_t flow);
bool hybrid_remote_capture_matches(uint32_t event_capture_id, uint32_t active_capture_id);
uint32_t hybrid_remote_stream_timeout_ms(hybrid_flow_t flow);

#endif

