#pragma once

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "mic_ws_client_state.h"

void mic_ws_tts_stream_flush_tail(mic_ws_state_t *state, portMUX_TYPE *lock);

void mic_ws_tts_stream_handle_binary(mic_ws_state_t *state,
                                     portMUX_TYPE *lock,
                                     const uint8_t *data,
                                     size_t len,
                                     const char *log_tag);
