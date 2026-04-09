#ifndef AUDIO_WORKER_RESAMPLER_H
#define AUDIO_WORKER_RESAMPLER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_worker_internal.h"

void audio_worker_resampler_reset(void);
esp_err_t audio_worker_resampler_write_fg(audio_worker_shared_state_t *state,
                                          const int16_t *samples,
                                          size_t sample_count,
                                          uint32_t src_rate_hz);

#endif
