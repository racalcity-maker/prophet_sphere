#ifndef AUDIO_DAC_BACKEND_H
#define AUDIO_DAC_BACKEND_H

#include "esp_err.h"

typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    esp_err_t (*start)(void);
    esp_err_t (*stop)(void);
} audio_dac_backend_t;

/* Selected at compile time by Kconfig DAC profile choice. */
extern const audio_dac_backend_t g_audio_dac_backend;

#endif
