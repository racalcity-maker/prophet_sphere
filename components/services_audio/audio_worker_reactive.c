#include "audio_worker_reactive.h"

#include <string.h>
#include "sdkconfig.h"
#include "app_tasking.h"
#include "audio_worker_internal.h"
#include "audio_reactive_analyzer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_ORB_AUDIO_REACTIVE_EVENT_ENABLE
#define CONFIG_ORB_AUDIO_REACTIVE_EVENT_ENABLE 1
#endif
#ifndef CONFIG_ORB_AUDIO_REACTIVE_UPDATE_MS
#define CONFIG_ORB_AUDIO_REACTIVE_UPDATE_MS 25
#endif

static esp_err_t post_audio_level(uint8_t level)
{
    app_event_t event = { 0 };
    event.id = APP_EVENT_AUDIO_LEVEL;
    event.source = APP_EVENT_SOURCE_AUDIO;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event.payload.scalar.value = level;
    return app_tasking_post_event(&event, 1U);
}

esp_err_t audio_worker_post_audio_done(uint32_t asset_id, int32_t code)
{
    app_event_t done = { 0 };
    done.id = APP_EVENT_AUDIO_DONE;
    done.source = APP_EVENT_SOURCE_AUDIO;
    done.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    done.payload.scalar.value = asset_id;
    done.payload.scalar.code = code;
    return app_tasking_post_event(&done, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t audio_worker_post_audio_error(uint32_t asset_id, int32_t code)
{
    app_event_t err_evt = { 0 };
    err_evt.id = APP_EVENT_AUDIO_ERROR;
    err_evt.source = APP_EVENT_SOURCE_AUDIO;
    err_evt.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    err_evt.payload.scalar.value = asset_id;
    err_evt.payload.scalar.code = code;
    return app_tasking_post_event(&err_evt, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

void audio_worker_audio_level_reset(bool post_zero)
{
#if CONFIG_ORB_AUDIO_REACTIVE_EVENT_ENABLE
    s_audio_level_filtered = 0U;
    s_audio_level_last_sent = 0U;
    s_audio_level_last_sent_tick = xTaskGetTickCount();
    s_audio_level_last_post_tick = xTaskGetTickCount();
    audio_reactive_analyzer_reset(&s_reactive_analyzer);
    if (post_zero) {
        (void)post_audio_level(0U);
    }
#else
    (void)post_zero;
#endif
}

void audio_worker_audio_level_process_samples(const int16_t *samples, size_t sample_count)
{
#if !CONFIG_ORB_AUDIO_REACTIVE_EVENT_ENABLE
    (void)samples;
    (void)sample_count;
    return;
#else
    audio_reactive_analyzer_process_pcm16_mono(&s_reactive_analyzer, samples, sample_count);
    s_audio_level_filtered = audio_reactive_analyzer_get_level(&s_reactive_analyzer);
#endif
}

void audio_worker_audio_level_maybe_publish(void)
{
#if !CONFIG_ORB_AUDIO_REACTIVE_EVENT_ENABLE
    return;
#else
    TickType_t now = xTaskGetTickCount();
    TickType_t publish_ticks = audio_worker_ms_to_ticks_min1((uint32_t)CONFIG_ORB_AUDIO_REACTIVE_UPDATE_MS);
    if ((now - s_audio_level_last_post_tick) < publish_ticks) {
        return;
    }
    s_audio_level_last_post_tick = now;

    if (s_audio_level_filtered == s_audio_level_last_sent && s_audio_level_filtered == 0U) {
        return;
    }

    uint8_t delta = (s_audio_level_filtered >= s_audio_level_last_sent)
                        ? (uint8_t)(s_audio_level_filtered - s_audio_level_last_sent)
                        : (uint8_t)(s_audio_level_last_sent - s_audio_level_filtered);
    TickType_t keepalive_ticks = audio_worker_ms_to_ticks_min1(120U);
    if (delta < 3U && (now - s_audio_level_last_sent_tick) < keepalive_ticks) {
        return;
    }

    if (post_audio_level(s_audio_level_filtered) == ESP_OK) {
        s_audio_level_last_sent = s_audio_level_filtered;
        s_audio_level_last_sent_tick = now;
    }
#endif
}
