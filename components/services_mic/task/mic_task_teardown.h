#pragma once

#include <stdbool.h>
#include "mic_task_types.h"

void mic_task_teardown_request_ws(mic_capture_ctx_t *ctx,
                                  mic_capture_ws_teardown_reason_t reason,
                                  esp_err_t ws_err);

void mic_task_teardown_apply_capture(mic_capture_ctx_t *ctx, bool task_stop, const char *log_tag);

