#include "mode_switch_orchestrator.h"

#include "control_dispatch.h"
#include "esp_log.h"
#include "log_tags.h"
#include "mode_switch_cleanup.h"
#include "submode_controller.h"

static const char *TAG = LOG_TAG_MODE_MANAGER;

static void try_runtime_rollback(mode_runtime_apply_hook_t runtime_hook, orb_mode_t from_mode, orb_mode_t to_mode)
{
    if (runtime_hook == NULL) {
        return;
    }
    esp_err_t rb_err = runtime_hook(from_mode, to_mode);
    if (rb_err != ESP_OK) {
        ESP_LOGW(TAG, "runtime rollback failed: %s", esp_err_to_name(rb_err));
    }
}

esp_err_t mode_switch_orchestrator_run(mode_switch_orchestrator_ctx_t *ctx,
                                       orb_mode_t previous_mode,
                                       orb_mode_t target_mode,
                                       const app_mode_t *previous_desc,
                                       const app_mode_t *target_desc)
{
    if (ctx == NULL || ctx->action_executor == NULL || ctx->mode_timers == NULL || target_desc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool runtime_applied = false;
    if (ctx->runtime_hook != NULL) {
        esp_err_t err = ctx->runtime_hook(previous_mode, target_mode);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "service runtime apply failed: %s", esp_err_to_name(err));
            return err;
        }
        runtime_applied = true;
    }

    if (previous_desc != NULL && previous_desc->exit != NULL) {
        esp_err_t err = previous_desc->exit();
        if (err != ESP_OK) {
            if (runtime_applied) {
                try_runtime_rollback(ctx->runtime_hook, target_mode, previous_mode);
            }
            ESP_LOGW(TAG, "mode exit failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    esp_err_t prep_err = mode_switch_cleanup_run(ctx->action_executor, ctx->mode_timers);
    if (prep_err != ESP_OK) {
        if (runtime_applied) {
            try_runtime_rollback(ctx->runtime_hook, target_mode, previous_mode);
        }
        ESP_LOGW(TAG, "mode switch cleanup failed: %s", esp_err_to_name(prep_err));
        return prep_err;
    }

    if (target_desc->enter != NULL) {
        esp_err_t err = target_desc->enter();
        if (err != ESP_OK) {
            if (runtime_applied) {
                try_runtime_rollback(ctx->runtime_hook, target_mode, previous_mode);
            }
            if (previous_desc != NULL && previous_desc->enter != NULL) {
                esp_err_t reenter_err = previous_desc->enter();
                if (reenter_err != ESP_OK) {
                    ESP_LOGW(TAG, "previous mode re-enter failed: %s", esp_err_to_name(reenter_err));
                }
            }
            ESP_LOGW(TAG, "mode enter failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    esp_err_t idle_scene_err = control_dispatch_queue_led_scene(submode_controller_idle_scene_for_mode(target_mode), 0U);
    if (idle_scene_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to queue idle scene for %s: %s", target_desc->name, esp_err_to_name(idle_scene_err));
    }

    return ESP_OK;
}
