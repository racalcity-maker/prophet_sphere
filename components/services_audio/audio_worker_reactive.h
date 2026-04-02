#ifndef AUDIO_WORKER_REACTIVE_H
#define AUDIO_WORKER_REACTIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_worker_post_audio_done(uint32_t asset_id, int32_t code);
esp_err_t audio_worker_post_audio_error(uint32_t asset_id, int32_t code);
void audio_worker_audio_level_reset(bool post_zero);
void audio_worker_audio_level_process_samples(const int16_t *samples, size_t sample_count);
void audio_worker_audio_level_maybe_publish(void);

#endif
