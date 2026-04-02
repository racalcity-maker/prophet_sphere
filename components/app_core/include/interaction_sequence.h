#ifndef INTERACTION_SEQUENCE_H
#define INTERACTION_SEQUENCE_H

#include <stdbool.h>
#include <stdint.h>
#include "app_events.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Centralized control-context helper for multi-track interaction flows.
 * Owns only sequence state/timers; audio playback is still owned by audio_task.
 */
esp_err_t interaction_sequence_init(void);
esp_err_t interaction_sequence_reset(void);

typedef esp_err_t (*interaction_sequence_before_second_hook_t)(uint32_t second_asset_id, uint32_t gap_ms);
esp_err_t interaction_sequence_set_before_second_hook(interaction_sequence_before_second_hook_t hook);

esp_err_t interaction_sequence_start_two_track(uint32_t first_asset_id, uint32_t second_asset_id, uint32_t gap_ms);

esp_err_t interaction_sequence_on_audio_done(uint32_t finished_asset_id, bool *consumed, bool *completed);
esp_err_t interaction_sequence_on_timer_expired(app_timer_kind_t timer_kind, bool *consumed);

#ifdef __cplusplus
}
#endif

#endif
