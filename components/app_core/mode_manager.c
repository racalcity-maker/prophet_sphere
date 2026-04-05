#include "mode_manager.h"

#include <string.h>
#include "app_mode.h"
#include "app_tasking.h"
#include "control_dispatch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "interaction_sequence.h"
#include "log_tags.h"
#include "mode_action_executor.h"
#include "mode_timers.h"
#include "sdkconfig.h"
#include "session_controller.h"
#include "submode_controller.h"

static const char *TAG = LOG_TAG_MODE_MANAGER;

#ifndef CONFIG_ORB_OFFLINE_GRUMBLE_FADE_OUT_MS
#define CONFIG_ORB_OFFLINE_GRUMBLE_FADE_OUT_MS 900
#endif

typedef struct {
    const app_mode_t *mode;
    bool registered;
} mode_slot_t;

static mode_slot_t s_modes[ORB_MODE_MAX];
static orb_mode_t s_current_mode = ORB_MODE_NONE;
static SemaphoreHandle_t s_mode_mutex;
static mode_runtime_apply_hook_t s_runtime_apply_hook;
static mode_action_executor_t s_action_executor;
static mode_timers_t s_mode_timers;

static TickType_t mode_lock_timeout_ticks(void)
{
    return pdMS_TO_TICKS(CONFIG_ORB_MODE_SWITCH_TIMEOUT_MS);
}

static bool mode_is_valid(orb_mode_t mode)
{
    return mode > ORB_MODE_NONE && mode < ORB_MODE_MAX;
}

static esp_err_t mode_lock(void)
{
    if (s_mode_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mode_mutex, mode_lock_timeout_ticks()) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void mode_unlock(void)
{
    if (s_mode_mutex != NULL) {
        xSemaphoreGive(s_mode_mutex);
    }
}

static const app_mode_t *get_mode_descriptor_unsafe(orb_mode_t mode)
{
    if (!mode_is_valid(mode)) {
        return NULL;
    }
    if (!s_modes[mode].registered) {
        return NULL;
    }
    return s_modes[mode].mode;
}

static orb_mode_t get_default_mode(void)
{
#if CONFIG_ORB_DEFAULT_MODE_HYBRID_AI
    return ORB_MODE_HYBRID_AI;
#elif CONFIG_ORB_DEFAULT_MODE_INSTALLATION_SLAVE
    return ORB_MODE_INSTALLATION_SLAVE;
#else
    return ORB_MODE_OFFLINE_SCRIPTED;
#endif
}

static app_mode_event_id_t map_event_id(app_event_id_t id)
{
    switch (id) {
    case APP_EVENT_TOUCH_DOWN:
        return APP_MODE_EVENT_TOUCH_DOWN;
    case APP_EVENT_TOUCH_UP:
        return APP_MODE_EVENT_TOUCH_UP;
    case APP_EVENT_TOUCH_HOLD:
        return APP_MODE_EVENT_TOUCH_HOLD;
    case APP_EVENT_AUDIO_DONE:
        return APP_MODE_EVENT_AUDIO_DONE;
    case APP_EVENT_AUDIO_ERROR:
        return APP_MODE_EVENT_AUDIO_ERROR;
    case APP_EVENT_MIC_CAPTURE_DONE:
        return APP_MODE_EVENT_MIC_CAPTURE_DONE;
    case APP_EVENT_MIC_ERROR:
        return APP_MODE_EVENT_MIC_ERROR;
    case APP_EVENT_MIC_REMOTE_PLAN_READY:
        return APP_MODE_EVENT_MIC_REMOTE_PLAN_READY;
    case APP_EVENT_MIC_REMOTE_PLAN_ERROR:
        return APP_MODE_EVENT_MIC_REMOTE_PLAN_ERROR;
    case APP_EVENT_MIC_TTS_STREAM_STARTED:
        return APP_MODE_EVENT_MIC_TTS_STREAM_STARTED;
    case APP_EVENT_MIC_TTS_DONE:
        return APP_MODE_EVENT_MIC_TTS_DONE;
    case APP_EVENT_MIC_TTS_ERROR:
        return APP_MODE_EVENT_MIC_TTS_ERROR;
    case APP_EVENT_AI_RESPONSE_READY:
        return APP_MODE_EVENT_AI_RESPONSE_READY;
    case APP_EVENT_AI_ERROR:
        return APP_MODE_EVENT_AI_ERROR;
    case APP_EVENT_TIMER_EXPIRED:
        return APP_MODE_EVENT_TIMER_EXPIRED;
    case APP_EVENT_NETWORK_UP:
        return APP_MODE_EVENT_NETWORK_UP;
    case APP_EVENT_NETWORK_DOWN:
        return APP_MODE_EVENT_NETWORK_DOWN;
    case APP_EVENT_MQTT_COMMAND_RX:
        return APP_MODE_EVENT_MQTT_COMMAND_RX;
    default:
        return APP_MODE_EVENT_UNKNOWN;
    }
}

static bool session_is_active(void)
{
    session_info_t info = { 0 };
    if (session_controller_get_info(&info) != ESP_OK) {
        return false;
    }
    return info.active;
}

static esp_err_t on_sequence_before_second(uint32_t second_asset_id, uint32_t gap_ms)
{
    return mode_action_executor_before_second_hook(&s_action_executor, second_asset_id, gap_ms);
}

const char *mode_manager_mode_to_str(orb_mode_t mode)
{
    const app_mode_t *desc = NULL;
    if (mode_lock() == ESP_OK) {
        desc = get_mode_descriptor_unsafe(mode);
        mode_unlock();
    }
    if (desc != NULL && desc->name != NULL) {
        return desc->name;
    }

    switch (mode) {
    case ORB_MODE_NONE:
        return "none";
    case ORB_MODE_OFFLINE_SCRIPTED:
        return "offline_scripted";
    case ORB_MODE_HYBRID_AI:
        return "hybrid_ai";
    case ORB_MODE_INSTALLATION_SLAVE:
        return "installation_slave";
    default:
        return "unknown";
    }
}

esp_err_t mode_manager_init(void)
{
    memset(s_modes, 0, sizeof(s_modes));
    s_current_mode = ORB_MODE_NONE;
    s_runtime_apply_hook = NULL;

    if (s_mode_mutex == NULL) {
        s_mode_mutex = xSemaphoreCreateMutex();
        if (s_mode_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(submode_controller_init(), TAG, "submode controller init failed");
    ESP_RETURN_ON_ERROR(mode_action_executor_init(&s_action_executor), TAG, "action executor init failed");
    ESP_RETURN_ON_ERROR(mode_timers_init(&s_mode_timers), TAG, "mode timers init failed");
    ESP_RETURN_ON_ERROR(interaction_sequence_init(), TAG, "sequence init failed");
    ESP_RETURN_ON_ERROR(interaction_sequence_set_before_second_hook(on_sequence_before_second),
                        TAG,
                        "sequence hook init failed");

    ESP_LOGI(TAG, "mode manager initialized");
    return ESP_OK;
}

esp_err_t mode_manager_set_runtime_apply_hook(mode_runtime_apply_hook_t hook)
{
    ESP_RETURN_ON_ERROR(mode_lock(), TAG, "mode lock failed");
    s_runtime_apply_hook = hook;
    mode_unlock();
    return ESP_OK;
}

esp_err_t mode_manager_register_mode(const struct app_mode *mode)
{
    if (mode == NULL || !mode_is_valid(mode->id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode->init != NULL) {
        ESP_RETURN_ON_ERROR(mode->init(), TAG, "mode init failed for %s", mode->name);
    }

    ESP_RETURN_ON_ERROR(mode_lock(), TAG, "mode lock failed");
    s_modes[mode->id].mode = mode;
    s_modes[mode->id].registered = true;
    mode_unlock();

    ESP_LOGI(TAG, "registered mode: %s", mode->name);
    return ESP_OK;
}

esp_err_t mode_manager_register_builtin_modes(void)
{
    ESP_RETURN_ON_ERROR(mode_manager_register_mode(mode_offline_scripted_get()), TAG, "register offline failed");
    ESP_RETURN_ON_ERROR(mode_manager_register_mode(mode_hybrid_ai_get()), TAG, "register hybrid failed");
    ESP_RETURN_ON_ERROR(mode_manager_register_mode(mode_installation_get()), TAG, "register installation failed");
    return ESP_OK;
}

esp_err_t mode_manager_activate_default_mode(void)
{
    return mode_manager_request_switch(get_default_mode());
}

bool mode_manager_is_registered(orb_mode_t mode)
{
    bool registered = false;
    if (!mode_is_valid(mode)) {
        return false;
    }
    if (mode_lock() != ESP_OK) {
        return false;
    }
    registered = s_modes[mode].registered;
    mode_unlock();
    return registered;
}

orb_mode_t mode_manager_get_current_mode(void)
{
    orb_mode_t mode = ORB_MODE_NONE;
    if (mode_lock() != ESP_OK) {
        return mode;
    }
    mode = s_current_mode;
    mode_unlock();
    return mode;
}

esp_err_t mode_manager_request_switch(orb_mode_t target_mode)
{
    if (!mode_is_valid(target_mode)) {
        return ESP_ERR_INVALID_ARG;
    }

    app_event_t event = { 0 };
    event.id = APP_EVENT_MODE_SWITCH_REQUEST;
    event.source = APP_EVENT_SOURCE_CONTROL;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event.payload.mode.from_mode = mode_manager_get_current_mode();
    event.payload.mode.to_mode = target_mode;

    return app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t mode_manager_request_network_reconfigure(void)
{
    app_event_t event = { 0 };
    event.id = APP_EVENT_NETWORK_RECONFIGURE_REQUEST;
    event.source = APP_EVENT_SOURCE_CONTROL;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

esp_err_t mode_manager_reconfigure_runtime_for_current_mode(void)
{
    mode_runtime_apply_hook_t runtime_hook = NULL;
    orb_mode_t current_mode = ORB_MODE_NONE;

    ESP_RETURN_ON_ERROR(mode_lock(), TAG, "mode lock failed");
    runtime_hook = s_runtime_apply_hook;
    current_mode = s_current_mode;
    mode_unlock();

    if (runtime_hook == NULL || current_mode == ORB_MODE_NONE) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "runtime reconfigure request in mode=%s", mode_manager_mode_to_str(current_mode));
    return runtime_hook(current_mode, current_mode);
}

esp_err_t mode_manager_perform_switch(orb_mode_t target_mode)
{
    if (!mode_is_valid(target_mode)) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(mode_lock(), TAG, "mode lock failed");
    orb_mode_t previous_mode = s_current_mode;
    const app_mode_t *target_desc = get_mode_descriptor_unsafe(target_mode);
    const app_mode_t *previous_desc = get_mode_descriptor_unsafe(previous_mode);
    mode_unlock();
    if (target_desc == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (previous_mode == target_mode) {
        return ESP_OK;
    }

    mode_runtime_apply_hook_t runtime_hook = NULL;
    if (mode_lock() == ESP_OK) {
        runtime_hook = s_runtime_apply_hook;
        mode_unlock();
    }

    ESP_LOGI(TAG,
             "mode switch start: %s -> %s",
             mode_manager_mode_to_str(previous_mode),
             mode_manager_mode_to_str(target_mode));

    bool runtime_applied = false;
    if (runtime_hook != NULL) {
        esp_err_t err = runtime_hook(previous_mode, target_mode);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "service runtime apply failed: %s", esp_err_to_name(err));
            return err;
        }
        runtime_applied = true;
    }

    if (previous_desc != NULL && previous_desc->exit != NULL) {
        esp_err_t err = previous_desc->exit();
        if (err != ESP_OK) {
            if (runtime_applied && runtime_hook != NULL) {
                esp_err_t rb_err = runtime_hook(target_mode, previous_mode);
                if (rb_err != ESP_OK) {
                    ESP_LOGW(TAG, "runtime rollback after exit failure failed: %s", esp_err_to_name(rb_err));
                }
            }
            ESP_LOGW(TAG, "mode exit failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_RETURN_ON_ERROR(mode_action_executor_prepare_for_mode_switch(&s_action_executor, &s_mode_timers),
                        TAG,
                        "prepare for mode switch failed");

    if (target_desc->enter != NULL) {
        esp_err_t err = target_desc->enter();
        if (err != ESP_OK) {
            if (runtime_applied && runtime_hook != NULL) {
                esp_err_t rb_err = runtime_hook(target_mode, previous_mode);
                if (rb_err != ESP_OK) {
                    ESP_LOGW(TAG, "runtime rollback after enter failure failed: %s", esp_err_to_name(rb_err));
                }
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

    ESP_RETURN_ON_ERROR(mode_lock(), TAG, "mode lock failed");
    s_current_mode = target_mode;
    mode_unlock();

    ESP_LOGI(TAG, "mode switch complete");
    return ESP_OK;
}

esp_err_t mode_manager_dispatch_event(const app_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    orb_mode_t current_mode = mode_manager_get_current_mode();
    bool suppress_touch_overlay = (current_mode == ORB_MODE_HYBRID_AI) ||
                                  submode_controller_is_offline_lottery_active(current_mode) ||
                                  mode_action_executor_should_suppress_touch_overlay(&s_action_executor, session_is_active());
    if (!suppress_touch_overlay) {
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

    bool consumed = false;
    ESP_RETURN_ON_ERROR(mode_action_executor_preprocess_event(&s_action_executor,
                                                              event,
                                                              submode_controller_idle_scene_for_mode(mode_manager_get_current_mode()),
                                                              (uint32_t)CONFIG_ORB_OFFLINE_GRUMBLE_FADE_OUT_MS,
                                                              &s_mode_timers,
                                                              &consumed),
                        TAG,
                        "event preprocess failed");
    if (consumed) {
        return ESP_OK;
    }

    if (event->id == APP_EVENT_AUDIO_DONE) {
        bool seq_consumed = false;
        bool completed = false;
        esp_err_t seq_err = interaction_sequence_on_audio_done(event->payload.scalar.value, &seq_consumed, &completed);
        if (seq_err != ESP_OK) {
            ESP_LOGW(TAG, "interaction sequence AUDIO_DONE handling failed: %s", esp_err_to_name(seq_err));
        }
        if (seq_consumed && !completed) {
            return ESP_OK;
        }
    } else if (event->id == APP_EVENT_TIMER_EXPIRED) {
        bool seq_consumed = false;
        esp_err_t seq_err =
            interaction_sequence_on_timer_expired((app_timer_kind_t)event->payload.scalar.code, &seq_consumed);
        if (seq_err != ESP_OK) {
            ESP_LOGW(TAG, "interaction sequence timer handling failed: %s", esp_err_to_name(seq_err));
        }
        if (seq_consumed) {
            return ESP_OK;
        }
    }

    const app_mode_t *mode = NULL;
    if (mode_lock() == ESP_OK) {
        mode = get_mode_descriptor_unsafe(s_current_mode);
        mode_unlock();
    }
    if (mode == NULL || mode->handle_event == NULL) {
        return ESP_OK;
    }

    app_mode_event_t mode_event = { 0 };
    mode_event.id = map_event_id(event->id);
    mode_event.code = 0;
    mode_event.aux = 0U;
    mode_event.confidence_permille = 0U;
    mode_event.intent_id = 0U;
    if (event->id == APP_EVENT_TOUCH_DOWN || event->id == APP_EVENT_TOUCH_UP || event->id == APP_EVENT_TOUCH_HOLD) {
        mode_event.value = event->payload.touch.zone_id;
    } else if (event->id == APP_EVENT_MQTT_COMMAND_RX) {
        (void)strncpy(mode_event.text, event->payload.text.text, sizeof(mode_event.text) - 1U);
        mode_event.text[sizeof(mode_event.text) - 1U] = '\0';
    } else if (event->id == APP_EVENT_TIMER_EXPIRED) {
        mode_event.code = event->payload.scalar.code;
        mode_event.value = (uint32_t)event->payload.scalar.code;
    } else if (event->id == APP_EVENT_AUDIO_DONE || event->id == APP_EVENT_AUDIO_ERROR) {
        mode_event.code = event->payload.scalar.code;
        mode_event.value = event->payload.scalar.value;
    } else if (event->id == APP_EVENT_MIC_CAPTURE_DONE ||
               event->id == APP_EVENT_MIC_REMOTE_PLAN_READY ||
               event->id == APP_EVENT_MIC_TTS_STREAM_STARTED) {
        mode_event.code = (int32_t)event->payload.mic.level_peak;
        mode_event.value = event->payload.mic.capture_id;
        mode_event.aux = (uint32_t)event->payload.mic.level_avg;
        mode_event.intent_id = event->payload.mic.intent_id;
        mode_event.confidence_permille = event->payload.mic.intent_confidence_permille;
    } else if (event->id == APP_EVENT_MIC_ERROR || event->id == APP_EVENT_MIC_REMOTE_PLAN_ERROR) {
        mode_event.code = event->payload.scalar.code;
        mode_event.value = event->payload.scalar.value;
    } else if (event->id == APP_EVENT_MIC_TTS_DONE || event->id == APP_EVENT_MIC_TTS_ERROR) {
        mode_event.code = event->payload.scalar.code;
        mode_event.value = event->payload.scalar.value;
    } else {
        mode_event.value = event->payload.scalar.value;
    }
    app_mode_action_t action = { 0 };

    ESP_RETURN_ON_ERROR(mode->handle_event(&mode_event, &action), TAG, "mode event handling failed");
    return mode_action_executor_handle_action(&s_action_executor, &action, &s_mode_timers);
}

esp_err_t mode_manager_handle_submode_request(void)
{
    return submode_controller_handle_request(mode_manager_get_current_mode());
}
