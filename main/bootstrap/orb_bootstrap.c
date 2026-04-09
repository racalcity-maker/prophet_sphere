#include "orb_bootstrap.h"

#include "orb_bootstrap_internal.h"

esp_err_t orb_bootstrap_start(void)
{
    esp_err_t err = orb_bootstrap_init_core();
    if (err != ESP_OK) {
        return err;
    }

    err = orb_bootstrap_init_runtime();
    if (err != ESP_OK) {
        return err;
    }

    err = orb_bootstrap_bind_inputs();
    if (err != ESP_OK) {
        return err;
    }

    orb_bootstrap_log_startup_snapshot();
    orb_bootstrap_apply_runtime_log_profile();
    err = orb_bootstrap_start_memory_monitor();
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}
