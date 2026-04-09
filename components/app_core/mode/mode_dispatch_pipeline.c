#include "mode_dispatch_pipeline.h"

#include <stdbool.h>
#include "control_dispatch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "interaction_sequence.h"
#include "log_tags.h"
#include "mode_event_adapter.h"
#include "sdkconfig.h"
#include "session_controller.h"
#include "submode_controller.h"

static const char *TAG = LOG_TAG_MODE_MANAGER;

static bool session_is_active(void)
{
    session_info_t info = { 0 };
    if (session_controller_get_info(&info) != ESP_OK) {
        return false;
    }
    return info.active;
}

static void process_touch_overlay(const mode_dispatch_pipeline_ctx_t *ctx, orb_mode_t current_mode, const app_event_t *event)
{
    bool suppress_touch_overlay = (current_mode == ORB_MODE_HYBRID_AI) ||
                                  submode_controller_is_offline_lottery_active(current_mode) ||
                                  mode_action_executor_should_suppress_touch_overlay(ctx->action_executor, session_is_active());
    if (suppress_touch_overlay) {
        return;
    }

    if (event->id == APP_EVENT_TOUCH_DOWN) {
        if (control_dispatch_queue_led_touch_zone(event->payload.touch.zone_id, true) != ESP_OK) {
            ESP_LOGW(TAG, "failed to queue touch DOWN overlay zone=%u", event->payload.touch.zone_id);
        }
    } else if (event->id == APP_EVENT_TOUCH_UP) {
        if (control_dispatch_queue_led_touch_zone(event->payload.touch.zone_id, false) != ESP_OK) {
            ESP_LOGW(TAG, "failed to queue touch UP overlay zone=%u", event->payload.touch.zone_id);
        }
    }
}

static esp_err_t preprocess_event(mode_dispatch_pipeline_ctx_t *ctx,
                                  orb_mode_t current_mode,
                                  const app_event_t *event,
                                  bool *consumed_out)
{
    bool consumed = false;
    if (consumed_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(mode_action_executor_preprocess_event(ctx->action_executor,
                                                              event,
                                                              submode_controller_idle_scene_for_mode(current_mode),
                                                              ctx->offline_grumble_fade_out_ms,
                                                              ctx->mode_timers,
                                                              &consumed),
                        TAG,
                        "event preprocess failed");
    *consumed_out = consumed;
    return ESP_OK;
}

static bool sequence_intercepted(const app_event_t *event)
{
    if (event->id == APP_EVENT_AUDIO_DONE) {
        bool seq_consumed = false;
        bool completed = false;
        esp_err_t seq_err = interaction_sequence_on_audio_done(event->payload.scalar.value, &seq_consumed, &completed);
        if (seq_err != ESP_OK) {
            ESP_LOGW(TAG, "interaction sequence AUDIO_DONE handling failed: %s", esp_err_to_name(seq_err));
        }
        return seq_consumed && !completed;
    }

    if (event->id == APP_EVENT_TIMER_EXPIRED) {
        bool seq_consumed = false;
        esp_err_t seq_err =
            interaction_sequence_on_timer_expired((app_timer_kind_t)event->payload.scalar.code, &seq_consumed);
        if (seq_err != ESP_OK) {
            ESP_LOGW(TAG, "interaction sequence timer handling failed: %s", esp_err_to_name(seq_err));
        }
        return seq_consumed;
    }

    return false;
}

esp_err_t mode_dispatch_pipeline_run(mode_dispatch_pipeline_ctx_t *ctx,
                                     orb_mode_t current_mode,
                                     const app_mode_t *mode,
                                     const app_event_t *event)
{
    if (ctx == NULL || ctx->action_executor == NULL || ctx->mode_timers == NULL || mode == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode->handle_event == NULL) {
        return ESP_OK;
    }

    process_touch_overlay(ctx, current_mode, event);

    bool consumed = false;
    ESP_RETURN_ON_ERROR(preprocess_event(ctx, current_mode, event, &consumed), TAG, "preprocess failed");
    if (consumed) {
        return ESP_OK;
    }

    if (sequence_intercepted(event)) {
        return ESP_OK;
    }

    app_mode_event_t mode_event = { 0 };
    mode_event_adapter_from_app_event(event, &mode_event);
    app_mode_action_t action = { 0 };

    ESP_RETURN_ON_ERROR(mode->handle_event(&mode_event, &action), TAG, "mode event handling failed");
    return mode_action_executor_handle_action(ctx->action_executor, &action, ctx->mode_timers);
}
