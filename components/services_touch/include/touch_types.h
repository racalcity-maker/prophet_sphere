#ifndef TOUCH_TYPES_H
#define TOUCH_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int8_t touch_hw_channel_t;

typedef enum {
    TOUCH_ZONE_0 = 0,
    TOUCH_ZONE_1,
    TOUCH_ZONE_2,
    TOUCH_ZONE_3,
    TOUCH_ZONE_COUNT,
    TOUCH_ZONE_INVALID = 0x7f,
} touch_zone_id_t;

typedef enum {
    TOUCH_STATE_IDLE = 0,
    TOUCH_STATE_ACTIVE,
} touch_state_t;

typedef struct {
    touch_zone_id_t id;
    touch_hw_channel_t channel;
    uint32_t raw;
    uint32_t filtered;
    uint32_t baseline;
    uint32_t delta;
    uint32_t threshold;
    touch_state_t state;
    bool pressed;
    bool hold_sent;
    uint8_t touch_count;
    uint8_t release_count;
    uint32_t pressed_since_ms;
} touch_zone_runtime_t;

typedef struct {
    touch_zone_runtime_t zones[TOUCH_ZONE_COUNT];
} touch_runtime_status_t;

typedef struct {
    uint32_t poll_period_ms;
    uint32_t calibration_samples;
    uint8_t debounce_count;
    uint8_t release_debounce_count;
    bool hold_event_enabled;
    uint32_t hold_time_ms;
    uint32_t threshold_percent;
    uint32_t threshold_min_delta;
    uint32_t release_threshold_percent;
    uint32_t filter_smooth_shift;
    uint32_t baseline_smooth_shift;
    uint32_t stuck_press_timeout_ms;
} touch_runtime_config_t;

#ifdef __cplusplus
}
#endif

#endif
