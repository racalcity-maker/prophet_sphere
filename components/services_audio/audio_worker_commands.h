#ifndef AUDIO_WORKER_COMMANDS_H
#define AUDIO_WORKER_COMMANDS_H

#include <stdbool.h>
#include <stdint.h>
#include "app_tasking.h"
#include "audio_mp3_helix.h"
#include "audio_worker_lifecycle.h"
#include "audio_worker_playback.h"
#include "audio_worker_internal.h"

typedef void (*audio_worker_commands_on_play_start_error_fn_t)(int32_t code);

typedef struct {
    audio_worker_lifecycle_state_t *lifecycle;
    audio_worker_shared_state_t *shared;
    audio_mp3_helix_ctx_t *mp3;
    audio_worker_playback_ctx_t *playback;
    TickType_t *pcm_stream_diag_last_log_tick;
    uint32_t *pcm_stream_rx_chunks;
    uint32_t *pcm_stream_rx_samples;
    bool *pcm_stream_chunk_written_since_poll;
    audio_worker_playback_ensure_output_fn_t ensure_output_started;
    audio_worker_playback_bg_prefill_fn_t bg_prefill;
    audio_worker_commands_on_play_start_error_fn_t on_play_start_error;
} audio_worker_commands_ctx_t;

void audio_worker_commands_init(audio_worker_commands_ctx_t *ctx,
                                audio_worker_lifecycle_state_t *lifecycle,
                                audio_worker_shared_state_t *shared,
                                audio_mp3_helix_ctx_t *mp3,
                                audio_worker_playback_ctx_t *playback,
                                TickType_t *pcm_stream_diag_last_log_tick,
                                uint32_t *pcm_stream_rx_chunks,
                                uint32_t *pcm_stream_rx_samples,
                                bool *pcm_stream_chunk_written_since_poll,
                                audio_worker_playback_ensure_output_fn_t ensure_output_started,
                                audio_worker_playback_bg_prefill_fn_t bg_prefill,
                                audio_worker_commands_on_play_start_error_fn_t on_play_start_error);

void audio_worker_commands_handle(audio_worker_commands_ctx_t *ctx, const audio_command_t *cmd);

#endif
