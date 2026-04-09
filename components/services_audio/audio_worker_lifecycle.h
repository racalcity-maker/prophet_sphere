#ifndef AUDIO_WORKER_LIFECYCLE_H
#define AUDIO_WORKER_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>
#include "audio_mp3_helix.h"
#include "audio_types.h"
#include "audio_worker_internal.h"

typedef enum {
    AUDIO_PLAYBACK_SOURCE_NONE = 0,
    AUDIO_PLAYBACK_SOURCE_SIMULATED,
    AUDIO_PLAYBACK_SOURCE_MP3_HELIX,
} audio_worker_playback_source_t;

typedef struct {
    audio_playback_state_t playback_state;
    audio_worker_playback_source_t playback_source;
    uint32_t active_asset_id;
    uint32_t sim_duration_ms;
    bool pcm_stop_drain_pending;
    uint32_t pcm_stop_drain_remaining_samples;
} audio_worker_lifecycle_state_t;

void audio_worker_lifecycle_reset(audio_worker_lifecycle_state_t *lifecycle);
void audio_worker_lifecycle_stop_foreground(audio_worker_lifecycle_state_t *lifecycle,
                                            audio_worker_shared_state_t *shared,
                                            audio_mp3_helix_ctx_t *mp3,
                                            bool post_zero_level);
void audio_worker_lifecycle_stop_all(audio_worker_lifecycle_state_t *lifecycle,
                                     audio_worker_shared_state_t *shared,
                                     audio_mp3_helix_ctx_t *mp3);
void audio_worker_lifecycle_begin_pcm_stop_drain_if_needed(audio_worker_lifecycle_state_t *lifecycle,
                                                           const audio_worker_shared_state_t *shared);
void audio_worker_lifecycle_pump_pcm_stop_drain(audio_worker_lifecycle_state_t *lifecycle,
                                                audio_worker_shared_state_t *shared);

#endif
