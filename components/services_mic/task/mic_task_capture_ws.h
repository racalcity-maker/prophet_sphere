#pragma once

#include <stddef.h>
#include <stdint.h>
#include "mic_task_types.h"

int16_t mic_task_capture_raw_to_pcm16(int32_t raw_sample);

void mic_task_capture_accumulate_metrics(mic_capture_ctx_t *ctx, const int32_t *samples, size_t sample_count);

void mic_task_capture_push_ws_chunk(mic_capture_ctx_t *ctx, const int32_t *samples, size_t sample_count, const char *log_tag);

void mic_task_capture_finalize_intent(mic_capture_ctx_t *ctx, const char *log_tag);
