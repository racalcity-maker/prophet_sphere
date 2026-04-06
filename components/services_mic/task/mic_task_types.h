#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "orb_intents.h"

typedef struct {
    bool active;
    uint32_t capture_id;
    TickType_t deadline_tick;
    uint64_t abs_sum;
    uint32_t sample_count;
    uint16_t peak;
    orb_intent_id_t intent_id;
    uint16_t intent_confidence_permille;
    bool ws_streaming;
    uint8_t ws_send_fail_streak;
    bool ws_result_used;
    esp_err_t ws_last_error;
} mic_capture_ctx_t;

typedef struct {
    bool active;
    uint32_t phase_accum;
} mic_loopback_ctx_t;
