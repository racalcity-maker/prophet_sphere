#ifndef AUDIO_WORKER_PLAYBACK_H
#define AUDIO_WORKER_PLAYBACK_H

#include <stdbool.h>
#include <stdint.h>
#include "audio_mp3_helix.h"
#include "audio_types.h"
#include "audio_worker_internal.h"
#include "audio_worker_lifecycle.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef esp_err_t (*audio_worker_playback_ensure_output_fn_t)(void);
typedef void (*audio_worker_playback_bg_prefill_fn_t)(void);
typedef void (*audio_worker_playback_pump_idle_fn_t)(void);

typedef struct {
    audio_worker_lifecycle_state_t *lifecycle;
    audio_worker_shared_state_t *shared;
    audio_mp3_helix_ctx_t *mp3;
    uint32_t *playback_session_id;
    TickType_t *playback_start_tick;
} audio_worker_playback_ctx_t;

typedef struct {
    bool done;
    bool error;
    int32_t error_code;
} audio_worker_playback_poll_result_t;

void audio_worker_playback_init(audio_worker_playback_ctx_t *ctx,
                                audio_worker_lifecycle_state_t *lifecycle,
                                audio_worker_shared_state_t *shared,
                                audio_mp3_helix_ctx_t *mp3,
                                uint32_t *playback_session_id,
                                TickType_t *playback_start_tick);

void audio_worker_playback_start_simulated(audio_worker_playback_ctx_t *ctx, uint32_t asset_id);

esp_err_t audio_worker_playback_start_mp3_or_fallback(audio_worker_playback_ctx_t *ctx,
                                                      uint32_t asset_id,
                                                      audio_worker_playback_ensure_output_fn_t ensure_output_started,
                                                      audio_worker_playback_bg_prefill_fn_t bg_prefill);

audio_worker_playback_poll_result_t audio_worker_playback_poll(audio_worker_playback_ctx_t *ctx,
                                                               audio_worker_playback_ensure_output_fn_t ensure_output_started,
                                                               audio_worker_playback_pump_idle_fn_t pump_background_only);

#endif
