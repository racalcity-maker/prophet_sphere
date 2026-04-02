#ifndef AUDIO_WORKER_BG_H
#define AUDIO_WORKER_BG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

void audio_worker_bg_reset_state(void);
void audio_worker_bg_close(void);
esp_err_t audio_worker_bg_start(uint32_t fade_in_ms, uint16_t gain_permille);
void audio_worker_bg_begin_fade(uint16_t target_gain_permille, uint32_t fade_ms, bool post_done_event);
size_t audio_worker_bg_read_samples(int16_t *out_samples, size_t sample_count);
void audio_worker_bg_update_fade(size_t consumed_samples);

#endif
