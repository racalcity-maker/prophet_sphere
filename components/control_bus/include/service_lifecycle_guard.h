#ifndef SERVICE_LIFECYCLE_GUARD_H
#define SERVICE_LIFECYCLE_GUARD_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Guard for service lifecycle ownership:
 * start/stop (and optional init) must be executed only from service_runtime path.
 */
esp_err_t service_lifecycle_guard_enter(void);
void service_lifecycle_guard_exit(void);
bool service_lifecycle_guard_is_owned(void);

#ifdef __cplusplus
}
#endif

#endif
