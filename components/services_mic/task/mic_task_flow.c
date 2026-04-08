#include "mic_task_flow.h"

#include <inttypes.h>
#include <stddef.h>
#include "esp_log.h"
#include "mic_i2s_hal.h"
#include "mic_task_events.h"
#include "mic_task_tts_pipeline.h"
#include "mic_ws_client.h"
#include "orb_intents.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_MIC_CAPTURE_DEFAULT_MS
#define CONFIG_ORB_MIC_CAPTURE_DEFAULT_MS 4000
#endif
#ifndef CONFIG_ORB_MIC_SAMPLE_RATE_HZ
#define CONFIG_ORB_MIC_SAMPLE_RATE_HZ 16000
#endif
#ifndef CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ
#define CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ 44100
#endif
#ifndef CONFIG_ORB_MIC_WS_ENABLE
#define CONFIG_ORB_MIC_WS_ENABLE 0
#endif

static uint32_t effective_capture_ms(uint32_t requested_ms)
{
    uint32_t capture_ms = requested_ms;
    if (capture_ms == 0U) {
        capture_ms = CONFIG_ORB_MIC_CAPTURE_DEFAULT_MS;
    }
    if (capture_ms < 50U) {
        capture_ms = 50U;
    }
    return capture_ms;
}

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static void play_tts_text(mic_capture_ctx_t *capture,
                          mic_loopback_ctx_t *loopback,
                          const char *text,
                          uint32_t stream_timeout_ms,
                          uint32_t bg_fade_out_ms,
                          const mic_task_flow_ops_t *ops)
{
    if (capture == NULL || loopback == NULL || text == NULL || text[0] == '\0' || ops == NULL ||
        ops->finish_capture == NULL || ops->stop_loopback == NULL) {
        return;
    }
    if (capture->active) {
        ops->finish_capture(capture, loopback, false);
    }
    if (loopback->active) {
        ops->stop_loopback(capture, loopback);
    }

    mic_task_tts_pipeline_play(capture->capture_id, text, stream_timeout_ms, bg_fade_out_ms);
}

static void start_capture(mic_capture_ctx_t *capture,
                          mic_loopback_ctx_t *loopback,
                          uint32_t capture_id,
                          uint32_t requested_ms,
                          const mic_task_flow_ops_t *ops,
                          const char *log_tag)
{
    if (capture == NULL || loopback == NULL || ops == NULL || ops->finish_capture == NULL ||
        ops->stop_loopback == NULL || ops->update_status == NULL) {
        return;
    }

    if (loopback->active) {
        ops->stop_loopback(capture, loopback);
    }
    if (capture->active) {
        ops->finish_capture(capture, loopback, false);
    }

    esp_err_t err = mic_i2s_hal_start();
    if (err != ESP_OK) {
        mic_task_events_post_capture_error(capture_id, err);
        return;
    }

    uint32_t capture_ms = effective_capture_ms(requested_ms);
    capture->active = true;
    capture->capture_id = capture_id;
    capture->abs_sum = 0U;
    capture->sample_count = 0U;
    capture->peak = 0U;
    capture->intent_id = ORB_INTENT_UNKNOWN;
    capture->intent_confidence_permille = 0U;
    capture->ws_streaming = false;
    capture->ws_send_fail_streak = 0U;
    capture->ws_result_used = false;
    capture->ws_last_error = ESP_OK;
    capture->ws_teardown_pending = false;
    capture->ws_teardown_reason = MIC_CAPTURE_WS_TEARDOWN_NONE;
    capture->deadline_tick = xTaskGetTickCount() + ms_to_ticks_min1(capture_ms);
    ops->update_status(capture, loopback->active);

#if CONFIG_ORB_MIC_WS_ENABLE
    err = mic_ws_client_session_start(capture_id, CONFIG_ORB_MIC_SAMPLE_RATE_HZ);
    if (err == ESP_OK) {
        capture->ws_streaming = true;
        capture->ws_send_fail_streak = 0U;
        ESP_LOGI(log_tag, "mic ws session start id=%" PRIu32, capture_id);
    } else {
        capture->ws_last_error = err;
        ESP_LOGW(log_tag, "mic ws unavailable (%s)", esp_err_to_name(err));
    }
#endif

    ESP_LOGI(log_tag, "capture start id=%" PRIu32 " duration=%" PRIu32 "ms", capture_id, capture_ms);
}

static void start_loopback(mic_capture_ctx_t *capture,
                           mic_loopback_ctx_t *loopback,
                           const mic_task_flow_ops_t *ops,
                           const char *log_tag)
{
    if (capture == NULL || loopback == NULL || ops == NULL || ops->finish_capture == NULL ||
        ops->update_status == NULL) {
        return;
    }

    if (capture->active) {
        ops->finish_capture(capture, loopback, false);
    }
    if (loopback->active) {
        return;
    }

    esp_err_t err = mic_i2s_hal_start();
    if (err != ESP_OK) {
        mic_task_events_post_capture_error(0U, err);
        return;
    }

    loopback->active = true;
    loopback->phase_accum = 0U;
    ops->update_status(capture, loopback->active);
    ESP_LOGI(log_tag,
             "loopback start mic=%dHz -> audio=%dHz",
             CONFIG_ORB_MIC_SAMPLE_RATE_HZ,
             CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ);
}

void mic_task_flow_process_command(mic_capture_ctx_t *capture,
                                   mic_loopback_ctx_t *loopback,
                                   const mic_command_t *cmd,
                                   const mic_task_flow_ops_t *ops,
                                   const char *log_tag)
{
    if (capture == NULL || loopback == NULL || cmd == NULL || ops == NULL || ops->finish_capture == NULL ||
        ops->stop_loopback == NULL || ops->update_status == NULL) {
        return;
    }

    switch (cmd->id) {
    case MIC_CMD_START_CAPTURE:
        start_capture(capture, loopback, cmd->payload.start_capture.capture_id, cmd->payload.start_capture.max_capture_ms, ops, log_tag);
        break;
    case MIC_CMD_STOP_CAPTURE:
        if (capture->active) {
            ESP_LOGI(log_tag, "capture stop id=%" PRIu32, capture->capture_id);
            ops->finish_capture(capture, loopback, false);
        }
        break;
    case MIC_CMD_LOOPBACK_START:
        start_loopback(capture, loopback, ops, log_tag);
        break;
    case MIC_CMD_LOOPBACK_STOP:
        ops->stop_loopback(capture, loopback);
        break;
    case MIC_CMD_TTS_PLAY_TEXT:
        play_tts_text(capture,
                      loopback,
                      cmd->payload.tts_play.text,
                      cmd->payload.tts_play.timeout_ms,
                      cmd->payload.tts_play.bg_fade_out_ms,
                      ops);
        break;
    case MIC_CMD_NONE:
    default:
        break;
    }
}
