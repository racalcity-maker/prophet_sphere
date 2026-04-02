#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "orb_intents.h"

void mic_task_events_post_capture_done(uint32_t capture_id,
                                       uint32_t sample_count,
                                       uint64_t abs_sum,
                                       uint16_t peak,
                                       orb_intent_id_t intent_id,
                                       uint16_t intent_confidence_permille);

void mic_task_events_post_capture_error(uint32_t capture_id, esp_err_t err);

void mic_task_events_post_remote_plan_ready(uint32_t capture_id,
                                            uint32_t sample_count,
                                            uint64_t abs_sum,
                                            uint16_t peak,
                                            orb_intent_id_t intent_id,
                                            uint16_t intent_confidence_permille);

void mic_task_events_post_remote_plan_error(uint32_t capture_id, esp_err_t err);

void mic_task_events_post_tts_stream_started(uint32_t capture_id, uint16_t started_marker);

void mic_task_events_post_tts_done(uint32_t dropped_chunks);

void mic_task_events_post_tts_error(esp_err_t err);
