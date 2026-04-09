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
    state->lottery_team_count = 2U;
    state->lottery_color_r[0] = 255U;
    state->lottery_color_g[0] = 60U;
    state->lottery_color_b[0] = 40U;
    state->lottery_color_r[1] = 40U;
    state->lottery_color_g[1] = 255U;
    state->lottery_color_b[1] = 120U;
    state->lottery_color_r[2] = 70U;
    state->lottery_color_g[2] = 160U;
    state->lottery_color_b[2] = 255U;
    state->lottery_color_r[3] = 255U;
    state->lottery_color_g[3] = 210U;
    state->lottery_color_b[3] = 40U;
    state->idle_v_q8 = 0U;
    state->hybrid_idle_v_q8 = 0U;
    state->hybrid_touch_v_q8 = 0U;
    state->hybrid_idle_hue_cycle = UINT32_MAX;
    state->hybrid_touch_hue_cycle = UINT32_MAX;
    state->hybrid_idle_hue = 0U;
    state->hybrid_touch_hue = 0U;
    state->rng_state = (seed == 0U) ? 0xC0FFEE11U : (seed ^ 0x9E3779B9U);
}
