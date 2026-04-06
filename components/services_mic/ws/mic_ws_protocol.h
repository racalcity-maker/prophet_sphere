#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "orb_intents.h"

bool mic_ws_protocol_parse_result_message(const char *json,
                                          uint32_t active_capture_id,
                                          uint32_t *out_capture_id,
                                          orb_intent_id_t *out_intent,
                                          uint16_t *out_conf);

bool mic_ws_protocol_parse_tts_control_message(const char *json, bool *out_done, bool *out_failed);

esp_err_t mic_ws_protocol_build_start_frame(uint32_t capture_id, uint32_t sample_rate_hz, char *out, size_t out_size);

esp_err_t mic_ws_protocol_build_end_frame(uint32_t capture_id, char *out, size_t out_size);

esp_err_t mic_ws_protocol_build_tts_request(const char *text,
                                            uint32_t sample_rate_hz,
                                            const char *voice,
                                            char *out,
                                            size_t out_size);
