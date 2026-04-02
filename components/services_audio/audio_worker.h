#ifndef AUDIO_WORKER_H
#define AUDIO_WORKER_H

#include <stdbool.h>
#include "app_tasking.h"

void audio_worker_init(void);
void audio_worker_handle_command(const audio_command_t *cmd);
void audio_worker_poll(void);
bool audio_worker_is_playing(void);

#endif
