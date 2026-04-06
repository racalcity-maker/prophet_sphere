#pragma once

#include <stdint.h>

void mic_task_tts_pipeline_play(uint32_t capture_id,
                                const char *text,
                                uint32_t stream_timeout_ms,
                                uint32_t bg_fade_out_ms);
