#ifndef AUDIO_TASK_H
#define AUDIO_TASK_H

#include "esp_err.h"

/*
 * Internal worker for audio command execution.
 * audio_task is the sole owner of playback execution state.
 */
esp_err_t audio_task_start(void);
esp_err_t audio_task_stop(void);

#endif
