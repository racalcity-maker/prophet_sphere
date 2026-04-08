#include "mic_task_capture_ws.h"

#include <inttypes.h>
#include <stddef.h>
#include "esp_log.h"
#include "mic_task_teardown.h"
#include "mic_ws_client.h"
#include "orb_intents.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_MIC_INPUT_SHIFT
#define CONFIG_ORB_MIC_INPUT_SHIFT 13
#endif
#ifndef CONFIG_ORB_MIC_SAMPLE_RATE_HZ
#define CONFIG_ORB_MIC_SAMPLE_RATE_HZ 16000
#endif
#ifndef CONFIG_ORB_MIC_READ_CHUNK_SAMPLES
#define CONFIG_ORB_MIC_READ_CHUNK_SAMPLES 256
#endif
#ifndef CONFIG_ORB_MIC_WS_RESULT_TIMEOUT_MS
#define CONFIG_ORB_MIC_WS_RESULT_TIMEOUT_MS 2000
#endif
#ifndef CONFIG_ORB_MIC_WS_ENABLE
#define CONFIG_ORB_MIC_WS_ENABLE 0
#endif

#define MIC_WS_DISABLE_FAIL_STREAK 80U
#define MIC_WS_LOG_EVERY_FAILS 10U
#define MIC_WS_RESYNC_MAX_ATTEMPTS 2U

static bool should_try_ws_resync(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE || err == ESP_FAIL || err == ESP_ERR_TIMEOUT;
}

static bool try_ws_resync_and_resend(mic_capture_ctx_t *ctx, const int16_t *samples, uint16_t sample_count)
{
    if (ctx == NULL || samples == NULL || sample_count == 0U) {
        return false;
    }

    for (uint32_t attempt = 0U; attempt < MIC_WS_RESYNC_MAX_ATTEMPTS; ++attempt) {
        esp_err_t start_err = mic_ws_client_session_start(ctx->capture_id, CONFIG_ORB_MIC_SAMPLE_RATE_HZ);
        if (start_err != ESP_OK) {
            continue;
        }
        esp_err_t resend_err = mic_ws_client_session_send_pcm16(samples, sample_count);
        if (resend_err == ESP_OK) {
            return true;
        }
    }
    return false;
}

static void handle_ws_send_fail(mic_capture_ctx_t *ctx,
                                esp_err_t err,
                                const int16_t *samples,
                                uint16_t sample_count,
                                const char *log_tag)
{
    if (ctx == NULL) {
        return;
    }

    if (should_try_ws_resync(err)) {
        bool recovered = try_ws_resync_and_resend(ctx, samples, sample_count);
        if (recovered) {
            ctx->ws_send_fail_streak = 0U;
            ESP_LOGW(log_tag, "mic ws stream recovered via resync (capture=%" PRIu32 ")", ctx->capture_id);
            return;
        }
    }

    ctx->ws_send_fail_streak++;
    if ((ctx->ws_send_fail_streak % MIC_WS_LOG_EVERY_FAILS) == 1U) {
        ESP_LOGW(log_tag,
                 "mic ws stream send fail streak=%u err=%s",
                 (unsigned)ctx->ws_send_fail_streak,
                 esp_err_to_name(err));
    }
    if (ctx->ws_send_fail_streak >= MIC_WS_DISABLE_FAIL_STREAK) {
        ctx->ws_streaming = false;
        ESP_LOGW(log_tag,
                 "mic ws stream disabled after failures streak=%u last_err=%s",
                 (unsigned)ctx->ws_send_fail_streak,
                 esp_err_to_name(err));
        mic_task_teardown_request_ws(ctx, MIC_CAPTURE_WS_TEARDOWN_CAPTURE_SEND_FAIL, err);
    }
}

int16_t mic_task_capture_raw_to_pcm16(int32_t raw_sample)
{
    int32_t shifted = (raw_sample >> CONFIG_ORB_MIC_INPUT_SHIFT);
    if (shifted > INT16_MAX) {
        shifted = INT16_MAX;
    } else if (shifted < INT16_MIN) {
        shifted = INT16_MIN;
    }
    return (int16_t)shifted;
}

void mic_task_capture_accumulate_metrics(mic_capture_ctx_t *ctx, const int32_t *samples, size_t sample_count)
{
    if (ctx == NULL || samples == NULL || sample_count == 0U) {
        return;
    }
    for (size_t i = 0; i < sample_count; ++i) {
        int16_t pcm16 = mic_task_capture_raw_to_pcm16(samples[i]);
        uint16_t abs_val = (pcm16 < 0) ? (uint16_t)(-pcm16) : (uint16_t)pcm16;
        ctx->abs_sum += (uint64_t)abs_val;
        if (abs_val > ctx->peak) {
            ctx->peak = abs_val;
        }
    }
    ctx->sample_count += (uint32_t)sample_count;
}

void mic_task_capture_push_ws_chunk(mic_capture_ctx_t *ctx, const int32_t *samples, size_t sample_count, const char *log_tag)
{
    if (ctx == NULL || !ctx->ws_streaming || samples == NULL || sample_count == 0U) {
        return;
    }

    int16_t pcm16_chunk[CONFIG_ORB_MIC_READ_CHUNK_SAMPLES];
    size_t used = 0U;

    for (size_t i = 0; i < sample_count; ++i) {
        pcm16_chunk[used++] = mic_task_capture_raw_to_pcm16(samples[i]);
        if (used == CONFIG_ORB_MIC_READ_CHUNK_SAMPLES) {
            esp_err_t err = mic_ws_client_session_send_pcm16(pcm16_chunk, (uint16_t)used);
            if (err != ESP_OK) {
                handle_ws_send_fail(ctx, err, pcm16_chunk, (uint16_t)used, log_tag);
                if (ctx->ws_send_fail_streak == 0U) {
                    used = 0U;
                    continue;
                }
                return;
            }
            ctx->ws_send_fail_streak = 0U;
            used = 0U;
        }
    }

    if (used > 0U) {
        esp_err_t err = mic_ws_client_session_send_pcm16(pcm16_chunk, (uint16_t)used);
        if (err != ESP_OK) {
            handle_ws_send_fail(ctx, err, pcm16_chunk, (uint16_t)used, log_tag);
            return;
        }
        ctx->ws_send_fail_streak = 0U;
    }
}

void mic_task_capture_finalize_intent(mic_capture_ctx_t *ctx, const char *log_tag)
{
    if (ctx == NULL) {
        return;
    }

    ctx->intent_id = ORB_INTENT_UNKNOWN;
    ctx->intent_confidence_permille = 0U;
    ctx->ws_result_used = false;
    if (!ctx->ws_teardown_pending) {
        ctx->ws_last_error = ESP_OK;
    }

    bool ws_used = false;
#if CONFIG_ORB_MIC_WS_ENABLE
    if (ctx->ws_streaming) {
        esp_err_t ws_err = mic_ws_client_session_finish();
        if (ws_err == ESP_OK) {
            orb_intent_id_t ws_intent = ORB_INTENT_UNKNOWN;
            uint16_t ws_conf = 0U;
            ws_err = mic_ws_client_take_result(ctx->capture_id,
                                               &ws_intent,
                                               &ws_conf,
                                               CONFIG_ORB_MIC_WS_RESULT_TIMEOUT_MS);
            if (ws_err == ESP_OK) {
                if (ws_intent != ORB_INTENT_UNKNOWN && ws_conf > 0U) {
                    ctx->intent_id = ws_intent;
                    ctx->intent_confidence_permille = ws_conf;
                    ctx->ws_result_used = true;
                    ws_used = true;
                    ESP_LOGI(log_tag, "kws final source=ws intent=%s conf=%u",
                             orb_intent_name(ctx->intent_id),
                             ctx->intent_confidence_permille);
                } else {
                    ctx->ws_last_error = ESP_ERR_NOT_FOUND;
                    ESP_LOGW(log_tag,
                             "kws ws result not usable intent=%s conf=%u -> wait timeout fallback",
                             orb_intent_name(ws_intent),
                             ws_conf);
                }
            } else {
                ESP_LOGW(log_tag, "mic ws result timeout/fail: %s", esp_err_to_name(ws_err));
                mic_task_teardown_request_ws(ctx, MIC_CAPTURE_WS_TEARDOWN_KWS_RESULT_FAIL, ws_err);
            }
        } else {
            ESP_LOGW(log_tag, "mic ws finish failed: %s", esp_err_to_name(ws_err));
            mic_task_teardown_request_ws(ctx, MIC_CAPTURE_WS_TEARDOWN_KWS_FINISH_FAIL, ws_err);
        }

        ctx->ws_streaming = false;
    }
#endif

    if (ws_used) {
        return;
    }
}
