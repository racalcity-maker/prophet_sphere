#include "led_effects.h"

#include <string.h>
#include <stdint.h>

void led_effects_reset_state(led_effects_state_t *state, uint32_t seed)
{
    if (state == NULL) {
        return;
    }
    memset(state->fire_heat, 0, sizeof(state->fire_heat));
    memset(state->sparkle_level, 0, sizeof(state->sparkle_level));
    state->aura_r = 32U;
    state->aura_g = 64U;
    state->aura_b = 255U;
    state->aura_level = 0U;
    state->idle_v_q8 = 0U;
    state->hybrid_idle_v_q8 = 0U;
    state->hybrid_touch_v_q8 = 0U;
    state->hybrid_idle_hue_cycle = UINT32_MAX;
    state->hybrid_touch_hue_cycle = UINT32_MAX;
    state->hybrid_idle_hue = 0U;
    state->hybrid_touch_hue = 0U;
    state->rng_state = (seed == 0U) ? 0xC0FFEE11U : (seed ^ 0x9E3779B9U);
}
