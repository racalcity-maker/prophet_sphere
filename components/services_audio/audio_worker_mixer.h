#ifndef AUDIO_WORKER_MIXER_H
#define AUDIO_WORKER_MIXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_worker_internal.h"

void audio_worker_fg_attack_reset(audio_worker_shared_state_t *state);
uint16_t audio_worker_fg_attack_next_permille(audio_worker_shared_state_t *state);
void audio_worker_pcm_stream_diag_reset(audio_worker_shared_state_t *state);
void audio_worker_pcm_stream_diag_snapshot(audio_worker_shared_state_t *state,
                                           uint32_t *out_jump_count,
                                           uint32_t *out_jump_max);
bool audio_worker_pcm_has_signal(const int16_t *samples, size_t sample_count, uint32_t abs_avg_threshold);
size_t audio_worker_compose_mixed_chunk(audio_worker_shared_state_t *state,
                                        const int16_t *fg_samples,
                                        size_t sample_count,
                                        bool has_foreground);
esp_err_t audio_worker_write_mixed_output(audio_worker_shared_state_t *state,
                                          const int16_t *fg_samples,
                                          size_t sample_count,
                                          bool has_foreground);

#endif
