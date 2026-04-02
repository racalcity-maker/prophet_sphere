#ifndef MIC_I2S_HAL_H
#define MIC_I2S_HAL_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mic_i2s_hal_init(void);
esp_err_t mic_i2s_hal_start(void);
esp_err_t mic_i2s_hal_stop(void);
esp_err_t mic_i2s_hal_deinit(void);
esp_err_t mic_i2s_hal_read_i32(int32_t *samples, size_t sample_count, size_t *out_samples, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
