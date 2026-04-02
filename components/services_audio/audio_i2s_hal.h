#ifndef AUDIO_I2S_HAL_H
#define AUDIO_I2S_HAL_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Shared low-level I2S TX transport layer.
 * DAC profile modules handle chip-specific GPIO/power behavior only.
 */
esp_err_t audio_i2s_hal_init(void);
esp_err_t audio_i2s_hal_start(void);
esp_err_t audio_i2s_hal_stop(void);
esp_err_t audio_i2s_hal_write_mono_pcm16(const int16_t *samples, size_t sample_count, uint32_t timeout_ms);

#endif
