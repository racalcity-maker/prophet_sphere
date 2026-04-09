#ifndef AUDIO_WORKER_BG_STREAM_H
#define AUDIO_WORKER_BG_STREAM_H

#include "audio_worker_internal.h"

void audio_worker_bg_stream_prefill_dma_queue(audio_worker_shared_state_t *shared);
void audio_worker_bg_stream_close_with_fade_done_if_needed(audio_worker_shared_state_t *shared, const char *reason);
void audio_worker_bg_stream_pump_background_only(audio_worker_shared_state_t *shared);

#endif
