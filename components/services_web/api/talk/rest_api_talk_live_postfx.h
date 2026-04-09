#ifndef REST_API_TALK_LIVE_POSTFX_H
#define REST_API_TALK_LIVE_POSTFX_H

#include <stdint.h>

typedef struct {
    int16_t last_sample;
    int32_t dc_prev_x;
    int32_t dc_prev_y;
    uint8_t has_last_sample;
} talk_live_postfx_state_t;

void talk_live_postfx_reset(talk_live_postfx_state_t *state);
void talk_live_postfx_apply_inplace(talk_live_postfx_state_t *state, int16_t *samples, uint32_t sample_count);

#endif

