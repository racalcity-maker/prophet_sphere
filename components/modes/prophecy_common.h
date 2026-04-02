#ifndef PROPHECY_COMMON_H
#define PROPHECY_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROPHECY_ARCHETYPE_CHOICE = 0,
    PROPHECY_ARCHETYPE_DANGER,
    PROPHECY_ARCHETYPE_FUTURE,
    PROPHECY_ARCHETYPE_INNER_STATE,
    PROPHECY_ARCHETYPE_LOVE,
    PROPHECY_ARCHETYPE_PATH,
    PROPHECY_ARCHETYPE_MONEY,
    PROPHECY_ARCHETYPE_WISH,
    PROPHECY_ARCHETYPE_YES_NO,
    PROPHECY_ARCHETYPE_TIMING,
    PROPHECY_ARCHETYPE_COUNT,
} prophecy_archetype_t;

typedef enum {
    PROPHECY_PHASE_GREET = 0,
    PROPHECY_PHASE_UNDERSTANDING,
    PROPHECY_PHASE_PREDICTION,
    PROPHECY_PHASE_FAREWELL,
    PROPHECY_PHASE_COUNT,
} prophecy_phase_t;

const char *prophecy_archetype_name(prophecy_archetype_t archetype);
const char *prophecy_phase_name(prophecy_phase_t phase);
prophecy_archetype_t prophecy_random_archetype(void);
uint32_t prophecy_asset_for(prophecy_archetype_t archetype, prophecy_phase_t phase);
bool prophecy_phase_next(prophecy_phase_t current_phase, prophecy_phase_t *out_next_phase);
bool prophecy_phase_advance(prophecy_archetype_t archetype,
                            prophecy_phase_t current_phase,
                            prophecy_phase_t *out_next_phase,
                            uint32_t *out_next_asset_id);
bool prophecy_asset_to_indices(uint32_t asset_id, uint8_t *out_archetype, uint8_t *out_phase);

#ifdef __cplusplus
}
#endif

#endif
