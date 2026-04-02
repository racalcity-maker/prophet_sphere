#ifndef MODE_MANAGER_H
#define MODE_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"
#include "app_defs.h"
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

struct app_mode;
typedef esp_err_t (*mode_runtime_apply_hook_t)(orb_mode_t previous_mode, orb_mode_t target_mode);

esp_err_t mode_manager_init(void);
esp_err_t mode_manager_register_mode(const struct app_mode *mode);
esp_err_t mode_manager_register_builtin_modes(void);
esp_err_t mode_manager_activate_default_mode(void);
bool mode_manager_is_registered(orb_mode_t mode);
esp_err_t mode_manager_set_runtime_apply_hook(mode_runtime_apply_hook_t hook);

/*
 * Queue-safe API, callable from callbacks/tasks.
 * It only posts a switch request into app_event_queue.
 */
esp_err_t mode_manager_request_switch(orb_mode_t target_mode);

/*
 * Control-context API. Must only be called from app_control_task context.
 * It performs centralized and safe mode switching.
 */
esp_err_t mode_manager_perform_switch(orb_mode_t target_mode);
esp_err_t mode_manager_dispatch_event(const app_event_t *event);
esp_err_t mode_manager_handle_submode_request(void);

/*
 * Queue-safe API for controlled runtime-level network reconfigure.
 * Used after STA credential updates; handled by app_control_task context.
 */
esp_err_t mode_manager_request_network_reconfigure(void);

/* Control-context API: app_control_task only. */
esp_err_t mode_manager_reconfigure_runtime_for_current_mode(void);

orb_mode_t mode_manager_get_current_mode(void);
const char *mode_manager_mode_to_str(orb_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
