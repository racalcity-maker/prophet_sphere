#ifndef SESSION_CONTROLLER_H
#define SESSION_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SESSION_IDLE = 0,
    SESSION_ACTIVATING,
    SESSION_SPEAKING,
    SESSION_COOLDOWN,
} session_state_t;

typedef struct {
    uint32_t session_id;
    session_state_t state;
    bool active;
} session_info_t;

esp_err_t session_controller_init(void);
esp_err_t session_controller_start_interaction(void);
esp_err_t session_controller_mark_speaking(void);
esp_err_t session_controller_begin_cooldown(uint32_t cooldown_ms);
esp_err_t session_controller_finish_cooldown(void);
esp_err_t session_controller_reset_to_idle(void);
esp_err_t session_controller_get_info(session_info_t *info);
const char *session_controller_state_to_str(session_state_t state);

#ifdef __cplusplus
}
#endif

#endif
