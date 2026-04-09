#ifndef AUDIO_WORKER_SIM_H
#define AUDIO_WORKER_SIM_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"

typedef struct audio_worker_shared_state audio_worker_shared_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void audio_worker_sim_init(audio_worker_shared_state_t *shared);
void audio_worker_sim_start(void);
void audio_worker_sim_pump(TickType_t playback_start_tick);
uint32_t audio_worker_sim_duration_ms(uint32_t fallback_duration_ms);

#ifdef __cplusplus
}
#endif

#endif
