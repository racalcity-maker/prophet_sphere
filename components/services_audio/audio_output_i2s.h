#ifndef AUDIO_OUTPUT_I2S_H
#define AUDIO_OUTPUT_I2S_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Low-level I2S output backend.
 * Owns codec-profile selection and I2S TX transport details.
 */
esp_err_t audio_output_i2s_init(void);
esp_err_t audio_output_i2s_start(void);
esp_err_t audio_output_i2s_resume_stream(void);
esp_err_t audio_output_i2s_pause_stream(void);
esp_err_t audio_output_i2s_stop(void);
esp_err_t audio_output_i2s_write_mono_pcm16(const int16_t *samples, size_t sample_count, uint32_t timeout_ms);

#endif
