#ifndef MODE_TIMERS_H
#define MODE_TIMERS_H

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    TimerHandle_t grumble_fade_timer;
    TimerHandle_t mode_audio_gap_timer;
    TimerHandle_t hybrid_ws_timeout_timer;
} mode_timers_t;

esp_err_t mode_timers_init(mode_timers_t *timers);
esp_err_t mode_timers_start_grumble_fade(mode_timers_t *timers, uint32_t delay_ms);
esp_err_t mode_timers_start_mode_audio_gap(mode_timers_t *timers, uint32_t delay_ms);
esp_err_t mode_timers_stop_mode_audio_gap(mode_timers_t *timers);
esp_err_t mode_timers_start_hybrid_ws_timeout(mode_timers_t *timers, uint32_t delay_ms);
esp_err_t mode_timers_stop_hybrid_ws_timeout(mode_timers_t *timers);
esp_err_t mode_timers_stop_all(mode_timers_t *timers);

#ifdef __cplusplus
}
#endif

#endif
