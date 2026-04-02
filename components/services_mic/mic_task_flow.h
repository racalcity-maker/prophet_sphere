#pragma once

#include <stdbool.h>
#include "mic_task.h"
#include "mic_task_types.h"

typedef struct {
    void (*finish_capture)(mic_capture_ctx_t *capture, mic_loopback_ctx_t *loopback, bool post_done);
    void (*stop_loopback)(mic_capture_ctx_t *capture, mic_loopback_ctx_t *loopback);
    void (*update_status)(const mic_capture_ctx_t *capture, bool loopback_active);
} mic_task_flow_ops_t;

void mic_task_flow_process_command(mic_capture_ctx_t *capture,
                                   mic_loopback_ctx_t *loopback,
                                   const mic_command_t *cmd,
                                   const mic_task_flow_ops_t *ops,
                                   const char *log_tag);
