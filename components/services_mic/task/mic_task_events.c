#include "mic_task_events.h"

#include <inttypes.h>
#include "app_events.h"
#include "app_tasking.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_MIC;

void mic_task_events_post_capture_done(uint32_t capture_id,
                                       uint32_t sample_count,
                                       uint64_t abs_sum,
                                       uint16_t peak,
                                       orb_intent_id_t intent_id,
                                       uint16_t intent_confidence_permille)
{
    app_event_t event = { 0 };
    event.id = APP_EVENT_MIC_CAPTURE_DONE;
    event.source = APP_EVENT_SOURCE_MIC;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event.payload.mic.capture_id = capture_id;
    event.payload.mic.level_peak = peak;
    event.payload.mic.intent_id = (uint8_t)intent_id;
    event.payload.mic.intent_confidence_permille = intent_confidence_permille;
    if (sample_count > 0U) {
        uint32_t avg = (uint32_t)(abs_sum / sample_count);
        if (avg > UINT16_MAX) {
            avg = UINT16_MAX;
        }
        event.payload.mic.level_avg = (uint16_t)avg;
    }

    if (app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "failed posting APP_EVENT_MIC_CAPTURE_DONE id=%" PRIu32, capture_id);
    } else {
        ESP_LOGI(TAG,
                 "capture done id=%" PRIu32 " samples=%" PRIu32 " avg=%u peak=%u intent=%s conf=%u",
                 capture_id,
                 sample_count,
                 event.payload.mic.level_avg,
                 event.payload.mic.level_peak,
                 orb_intent_name(intent_id),
                 (unsigned)intent_confidence_permille);
    }
}

void mic_task_events_post_capture_error(uint32_t capture_id, esp_err_t err)
{
    app_event_t event = { 0 };
    event.id = APP_EVENT_MIC_ERROR;
    event.source = APP_EVENT_SOURCE_MIC;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event.payload.scalar.value = capture_id;
    event.payload.scalar.code = (int32_t)err;

    if (app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "failed posting APP_EVENT_MIC_ERROR id=%" PRIu32 " err=%s", capture_id, esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "capture error id=%" PRIu32 " err=%s", capture_id, esp_err_to_name(err));
    }
}

void mic_task_events_post_remote_plan_ready(uint32_t capture_id,
                                            uint32_t sample_count,
                                            uint64_t abs_sum,
                                            uint16_t peak,
                                            orb_intent_id_t intent_id,
                                            uint16_t intent_confidence_permille)
{
    app_event_t event = { 0 };
    event.id = APP_EVENT_MIC_REMOTE_PLAN_READY;
    event.source = APP_EVENT_SOURCE_MIC;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event.payload.mic.capture_id = capture_id;
    event.payload.mic.level_peak = peak;
    event.payload.mic.intent_id = (uint8_t)intent_id;
    event.payload.mic.intent_confidence_permille = intent_confidence_permille;
    if (sample_count > 0U) {
        uint32_t avg = (uint32_t)(abs_sum / sample_count);
        if (avg > UINT16_MAX) {
            avg = UINT16_MAX;
        }
        event.payload.mic.level_avg = (uint16_t)avg;
    }

    if (app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "failed posting APP_EVENT_MIC_REMOTE_PLAN_READY id=%" PRIu32, capture_id);
    }
}

void mic_task_events_post_remote_plan_error(uint32_t capture_id, esp_err_t err)
{
    app_event_t event = { 0 };
    event.id = APP_EVENT_MIC_REMOTE_PLAN_ERROR;
    event.source = APP_EVENT_SOURCE_MIC;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event.payload.scalar.value = capture_id;
    event.payload.scalar.code = (int32_t)err;

    if (app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "failed posting APP_EVENT_MIC_REMOTE_PLAN_ERROR id=%" PRIu32 " err=%s", capture_id, esp_err_to_name(err));
    }
}

void mic_task_events_post_tts_stream_started(uint32_t capture_id)
{
    app_event_t event = { 0 };
    event.id = APP_EVENT_MIC_TTS_STREAM_STARTED;
    event.source = APP_EVENT_SOURCE_MIC;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event.payload.mic.capture_id = capture_id;
    event.payload.mic.level_avg = 0U;
    event.payload.mic.level_peak = 0U;
    event.payload.mic.intent_id = (uint8_t)ORB_INTENT_UNKNOWN;
    event.payload.mic.intent_confidence_permille = 0U;

    if (app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "failed posting APP_EVENT_MIC_TTS_STREAM_STARTED");
    }
}

void mic_task_events_post_tts_done(uint32_t dropped_chunks)
{
    app_event_t event = { 0 };
    event.id = APP_EVENT_MIC_TTS_DONE;
    event.source = APP_EVENT_SOURCE_MIC;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event.payload.scalar.value = dropped_chunks;
    event.payload.scalar.code = 0;

    if (app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "failed posting APP_EVENT_MIC_TTS_DONE");
    }
}

void mic_task_events_post_tts_error(esp_err_t err)
{
    app_event_t event = { 0 };
    event.id = APP_EVENT_MIC_TTS_ERROR;
    event.source = APP_EVENT_SOURCE_MIC;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event.payload.scalar.value = 0U;
    event.payload.scalar.code = (int32_t)err;

    if (app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "failed posting APP_EVENT_MIC_TTS_ERROR err=%s", esp_err_to_name(err));
    }
}
