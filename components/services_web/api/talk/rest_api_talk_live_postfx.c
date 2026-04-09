#include "rest_api_talk_live_postfx.h"

#include <stddef.h>
#include "rest_api_talk_internal.h"

static inline int16_t talk_live_clamp_i16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

static int16_t talk_live_postfx_soft_limit(int32_t x)
{
    int32_t ax = (x < 0) ? -x : x;
    if (ax <= TALK_LIVE_POSTFX_LIMITER_THRESHOLD) {
        return talk_live_clamp_i16(x);
    }
    int32_t over = ax - TALK_LIVE_POSTFX_LIMITER_THRESHOLD;
    int32_t comp = TALK_LIVE_POSTFX_LIMITER_THRESHOLD;
    comp += (int32_t)(((int64_t)over * (int64_t)TALK_LIVE_POSTFX_LIMITER_KNEE) /
                      (int64_t)(TALK_LIVE_POSTFX_LIMITER_KNEE + over));
    if (comp > 32767) {
        comp = 32767;
    }
    return (x < 0) ? (int16_t)(-comp) : (int16_t)comp;
}

void talk_live_postfx_reset(talk_live_postfx_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->has_last_sample = 0U;
    state->last_sample = 0;
    state->dc_prev_x = 0;
    state->dc_prev_y = 0;
}

void talk_live_postfx_apply_inplace(talk_live_postfx_state_t *state, int16_t *samples, uint32_t sample_count)
{
    if (state == NULL || samples == NULL || sample_count == 0U) {
        return;
    }

    if (state->has_last_sample != 0U) {
        int32_t first = (int32_t)samples[0];
        int32_t prev = (int32_t)state->last_sample;
        int32_t jump = first - prev;
        if (jump < 0) {
            jump = -jump;
        }
        if (jump >= TALK_LIVE_POSTFX_DECLICK_THRESHOLD) {
            uint32_t ramp_n = sample_count;
            if (ramp_n > TALK_LIVE_POSTFX_DECLICK_RAMP_SAMPLES) {
                ramp_n = TALK_LIVE_POSTFX_DECLICK_RAMP_SAMPLES;
            }
            for (uint32_t i = 0U; i < ramp_n; ++i) {
                int32_t target = (int32_t)samples[i];
                int32_t mixed = prev + (int32_t)(((int64_t)(target - prev) * (int64_t)(i + 1U)) / (int64_t)ramp_n);
                samples[i] = talk_live_clamp_i16(mixed);
            }
        }
    }

    int32_t prev_x = state->dc_prev_x;
    int32_t prev_y = state->dc_prev_y;
    for (uint32_t i = 0U; i < sample_count; ++i) {
        int32_t x = (int32_t)samples[i];
        int32_t y = (x - prev_x) +
                    (int32_t)(((int64_t)TALK_LIVE_POSTFX_DC_BETA_Q15 * (int64_t)prev_y) >> 15);
        prev_x = x;
        prev_y = y;
        samples[i] = talk_live_postfx_soft_limit(y);
    }

    state->dc_prev_x = prev_x;
    state->dc_prev_y = prev_y;
    state->last_sample = samples[sample_count - 1U];
    state->has_last_sample = 1U;
}

