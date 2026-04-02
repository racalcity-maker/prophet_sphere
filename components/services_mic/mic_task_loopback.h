#pragma once

#include <stddef.h>
#include <stdint.h>

void mic_task_loopback_stream_samples(uint32_t *phase_accum, const int32_t *samples, size_t sample_count, const char *log_tag);
