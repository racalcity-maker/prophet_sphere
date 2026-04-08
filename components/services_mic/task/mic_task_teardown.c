#include "mic_task_teardown.h"

#include "esp_log.h"
#include "mic_ws_client.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_MIC_WS_ENABLE
#define CONFIG_ORB_MIC_WS_ENABLE 0
#endif

static mic_ws_close_reason_t map_reason(mic_capture_ws_teardown_reason_t reason)
{
    switch (reason) {
    case MIC_CAPTURE_WS_TEARDOWN_CAPTURE_SEND_FAIL:
        return MIC_WS_CLOSE_REASON_CAPTURE_SEND_FAIL;
    case MIC_CAPTURE_WS_TEARDOWN_KWS_FINISH_FAIL:
        return MIC_WS_CLOSE_REASON_KWS_FINISH_FAIL;
    case MIC_CAPTURE_WS_TEARDOWN_KWS_RESULT_FAIL:
        return MIC_WS_CLOSE_REASON_KWS_RESULT_FAIL;
    case MIC_CAPTURE_WS_TEARDOWN_NONE:
    default:
        return MIC_WS_CLOSE_REASON_UNSPECIFIED;
    }
}

void mic_task_teardown_request_ws(mic_capture_ctx_t *ctx,
                                  mic_capture_ws_teardown_reason_t reason,
                                  esp_err_t ws_err)
{
    if (ctx == NULL || reason == MIC_CAPTURE_WS_TEARDOWN_NONE) {
        return;
    }

    if (ws_err != ESP_OK) {
        ctx->ws_last_error = ws_err;
    }
    if (!ctx->ws_teardown_pending) {
        ctx->ws_teardown_pending = true;
        ctx->ws_teardown_reason = reason;
    }
}

void mic_task_teardown_apply_capture(mic_capture_ctx_t *ctx, bool task_stop, const char *log_tag)
{
    if (ctx == NULL) {
        return;
    }
    if (log_tag == NULL) {
        log_tag = "mic_task";
    }

#if CONFIG_ORB_MIC_WS_ENABLE
    if (ctx->ws_teardown_pending) {
        mic_ws_close_reason_t close_reason = map_reason(ctx->ws_teardown_reason);
        if (close_reason != MIC_WS_CLOSE_REASON_UNSPECIFIED) {
            ESP_LOGI(log_tag, "mic ws teardown apply reason=%d", (int)close_reason);
            mic_ws_client_fail_current_session(close_reason);
        }
    } else if (task_stop) {
        ESP_LOGI(log_tag, "mic ws teardown apply reason=%d", (int)MIC_WS_CLOSE_REASON_TASK_STOP);
        mic_ws_client_fail_current_session(MIC_WS_CLOSE_REASON_TASK_STOP);
    }
#else
    (void)task_stop;
    (void)log_tag;
#endif

    ctx->ws_teardown_pending = false;
    ctx->ws_teardown_reason = MIC_CAPTURE_WS_TEARDOWN_NONE;
}
