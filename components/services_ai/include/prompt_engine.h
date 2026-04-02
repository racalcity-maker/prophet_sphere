#ifndef PROMPT_ENGINE_H
#define PROMPT_ENGINE_H

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t prompt_engine_init(void);
esp_err_t prompt_engine_compose(const char *input, char *out_buf, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif
