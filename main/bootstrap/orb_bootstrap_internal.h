#ifndef ORB_BOOTSTRAP_INTERNAL_H
#define ORB_BOOTSTRAP_INTERNAL_H

#include "esp_err.h"

esp_err_t orb_bootstrap_init_core(void);
esp_err_t orb_bootstrap_init_runtime(void);
esp_err_t orb_bootstrap_bind_inputs(void);
void orb_bootstrap_log_startup_snapshot(void);
void orb_bootstrap_apply_runtime_log_profile(void);
esp_err_t orb_bootstrap_start_memory_monitor(void);

#endif
