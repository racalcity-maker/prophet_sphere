#include "mode_timers.h"

#include "app_tasking.h"
#include "sdkconfig.h"

static void grumble_fade_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    (void)app_tasking_post_timer_event_reliable(APP_TIMER_KIND_GRUMBLE_FADE, 0U);
}

static void mode_audio_gap_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    (void)app_tasking_post_timer_event_reliable(APP_TIMER_KIND_MODE_AUDIO_GAP, 0U);
}

static void hybrid_ws_timeout_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    (void)app_tasking_post_timer_event_reliable(APP_TIMER_KIND_HYBRID_WS_TIMEOUT, 0U);
}

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0U) ? ticks : 1U;
}

static esp_err_t start_one_shot(TimerHandle_t timer, uint32_t delay_ms)
{
    if (timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    TickType_t period_ticks = ms_to_ticks_min1(delay_ms);

    if (xTimerStop(timer, timeout_ticks) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    if (xTimerChangePeriod(timer, period_ticks, timeout_ticks) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    if (xTimerStart(timer, timeout_ticks) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t mode_timers_init(mode_timers_t *timers)
{
    if (timers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (timers->grumble_fade_timer == NULL) {
        timers->grumble_fade_timer = xTimerCreate("grm_fade",
                                                  ms_to_ticks_min1(10U),
                                                  pdFALSE,
                                                  NULL,
                                                  grumble_fade_timer_cb);
        if (timers->grumble_fade_timer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (timers->mode_audio_gap_timer == NULL) {
        timers->mode_audio_gap_timer = xTimerCreate("mode_a_gap",
                                                    ms_to_ticks_min1(10U),
                                                    pdFALSE,
                                                    NULL,
                                                    mode_audio_gap_timer_cb);
        if (timers->mode_audio_gap_timer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (timers->hybrid_ws_timeout_timer == NULL) {
        timers->hybrid_ws_timeout_timer = xTimerCreate("hyb_ws_to",
                                                       ms_to_ticks_min1(10U),
                                                       pdFALSE,
                                                       NULL,
                                                       hybrid_ws_timeout_timer_cb);
        if (timers->hybrid_ws_timeout_timer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t mode_timers_start_grumble_fade(mode_timers_t *timers, uint32_t delay_ms)
{
    if (timers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return start_one_shot(timers->grumble_fade_timer, delay_ms);
}

esp_err_t mode_timers_start_mode_audio_gap(mode_timers_t *timers, uint32_t delay_ms)
{
    if (timers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return start_one_shot(timers->mode_audio_gap_timer, delay_ms);
}

esp_err_t mode_timers_stop_mode_audio_gap(mode_timers_t *timers)
{
    if (timers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timers->mode_audio_gap_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTimerStop(timers->mode_audio_gap_timer, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t mode_timers_start_hybrid_ws_timeout(mode_timers_t *timers, uint32_t delay_ms)
{
    if (timers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return start_one_shot(timers->hybrid_ws_timeout_timer, delay_ms);
}

esp_err_t mode_timers_stop_hybrid_ws_timeout(mode_timers_t *timers)
{
    if (timers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timers->hybrid_ws_timeout_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTimerStop(timers->hybrid_ws_timeout_timer, pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t mode_timers_stop_all(mode_timers_t *timers)
{
    if (timers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    if (timers->grumble_fade_timer != NULL) {
        (void)xTimerStop(timers->grumble_fade_timer, timeout_ticks);
    }
    if (timers->mode_audio_gap_timer != NULL) {
        (void)xTimerStop(timers->mode_audio_gap_timer, timeout_ticks);
    }
    if (timers->hybrid_ws_timeout_timer != NULL) {
        (void)xTimerStop(timers->hybrid_ws_timeout_timer, timeout_ticks);
    }
    return ESP_OK;
}
