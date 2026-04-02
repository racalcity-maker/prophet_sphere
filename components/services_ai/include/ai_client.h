#ifndef AI_CLIENT_H
#define AI_CLIENT_H

#include <stdint.h>
#include "esp_err.h"
#include "ai_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Queue-based and thread-safe frontend.
 * ai_task is the only owner of AI request execution state.
 */
esp_err_t ai_client_init(void);
esp_err_t ai_client_start_task(void);
esp_err_t ai_client_stop_task(void);
esp_err_t ai_client_request(uint32_t request_id, const char *prompt, uint32_t timeout_ms);
esp_err_t ai_client_cancel(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
