#include "service_runtime.h"

#include <inttypes.h>
#include <stdbool.h>
#include "ai_client.h"
#include "audio_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_service.h"
#include "log_tags.h"
#include "mqtt_service.h"
#include "mic_service.h"
#include "network_manager.h"
#include "ota_service.h"
#include "sdkconfig.h"
#include "service_lifecycle_guard.h"
#include "storage_manager.h"
#include "touch_service.h"
#include "web_server.h"

static const char *TAG = LOG_TAG_SERVICE_RUNTIME;

typedef esp_err_t (*service_lifecycle_fn_t)(void);

typedef struct {
    const char *name;
    service_lifecycle_fn_t init_fn;
    service_lifecycle_fn_t start_fn;
    service_lifecycle_fn_t stop_fn;
    bool stop_supported;
    bool start_required;
    bool restart_while_running;
} service_desc_t;

typedef struct {
    service_runtime_state_t state;
} service_ctx_t;

static SemaphoreHandle_t s_runtime_mutex;
static SemaphoreHandle_t s_transition_mutex;
static service_ctx_t s_service_ctx[SERVICE_RUNTIME_COUNT];
static service_runtime_requirements_t s_active_requirements;

static esp_err_t start_touch(void)
{
    return touch_service_start_task();
}

static esp_err_t start_led(void)
{
    return led_service_start_task();
}

static esp_err_t start_audio(void)
{
    return audio_service_start_task();
}

static esp_err_t start_mic(void)
{
    return mic_service_start_task();
}

static esp_err_t start_ai(void)
{
    return ai_client_start_task();
}

static const service_desc_t s_services[SERVICE_RUNTIME_COUNT] = {
    [SERVICE_RUNTIME_TOUCH] = {
        .name = "touch",
        .init_fn = touch_service_init,
        .start_fn = start_touch,
        .stop_fn = NULL,
        .stop_supported = false,
        .start_required = true,
        .restart_while_running = false,
    },
    [SERVICE_RUNTIME_LED] = {
        .name = "led",
        .init_fn = led_service_init,
        .start_fn = start_led,
        .stop_fn = NULL,
        .stop_supported = false,
        .start_required = true,
        .restart_while_running = false,
    },
    [SERVICE_RUNTIME_AUDIO] = {
        .name = "audio",
        .init_fn = audio_service_init,
        .start_fn = start_audio,
        .stop_fn = NULL,
        .stop_supported = false,
        .start_required = true,
        .restart_while_running = false,
    },
    [SERVICE_RUNTIME_MIC] = {
        .name = "mic",
        .init_fn = mic_service_init,
        .start_fn = start_mic,
        .stop_fn = mic_service_stop_task,
        .stop_supported = true,
        .start_required = false,
        .restart_while_running = false,
    },
    [SERVICE_RUNTIME_AI] = {
        .name = "ai",
        .init_fn = ai_client_init,
        .start_fn = start_ai,
        .stop_fn = ai_client_stop_task,
        .stop_supported = true,
        .start_required = false,
        .restart_while_running = false,
    },
    [SERVICE_RUNTIME_STORAGE] = {
        .name = "storage",
        .init_fn = storage_manager_init,
        .start_fn = storage_manager_mount,
        .stop_fn = storage_manager_unmount,
        .stop_supported = true,
        .start_required = false,
        .restart_while_running = false,
    },
    [SERVICE_RUNTIME_NETWORK] = {
        .name = "network",
        .init_fn = network_manager_init,
        .start_fn = network_manager_start,
        .stop_fn = network_manager_stop,
        .stop_supported = true,
        .start_required = false,
        .restart_while_running = true,
    },
    [SERVICE_RUNTIME_MQTT] = {
        .name = "mqtt",
        .init_fn = mqtt_service_init,
        .start_fn = mqtt_service_start,
        .stop_fn = mqtt_service_stop,
        .stop_supported = true,
        .start_required = false,
        .restart_while_running = false,
    },
    [SERVICE_RUNTIME_WEB] = {
        .name = "web",
        .init_fn = web_server_init,
        .start_fn = web_server_start,
        .stop_fn = web_server_stop,
        .stop_supported = true,
        .start_required = false,
        .restart_while_running = false,
    },
    [SERVICE_RUNTIME_OTA] = {
        .name = "ota",
        .init_fn = ota_service_init,
        .start_fn = ota_service_start,
        .stop_fn = ota_service_stop,
        .stop_supported = true,
        .start_required = false,
        .restart_while_running = false,
    },
};

static const service_runtime_id_t s_start_order[] = {
    SERVICE_RUNTIME_STORAGE,
    SERVICE_RUNTIME_TOUCH,
    SERVICE_RUNTIME_LED,
    SERVICE_RUNTIME_AUDIO,
    SERVICE_RUNTIME_MIC,
    SERVICE_RUNTIME_NETWORK,
    SERVICE_RUNTIME_MQTT,
    SERVICE_RUNTIME_WEB,
    SERVICE_RUNTIME_AI,
    SERVICE_RUNTIME_OTA,
};

static const service_runtime_id_t s_stop_order[] = {
    SERVICE_RUNTIME_OTA,
    SERVICE_RUNTIME_AI,
    SERVICE_RUNTIME_WEB,
    SERVICE_RUNTIME_MQTT,
    SERVICE_RUNTIME_NETWORK,
    SERVICE_RUNTIME_MIC,
    SERVICE_RUNTIME_AUDIO,
    SERVICE_RUNTIME_LED,
    SERVICE_RUNTIME_TOUCH,
    SERVICE_RUNTIME_STORAGE,
};

static TickType_t runtime_lock_timeout_ticks(void)
{
    return pdMS_TO_TICKS(CONFIG_ORB_MODE_SWITCH_TIMEOUT_MS);
}

static esp_err_t lock_runtime(void)
{
    if (s_runtime_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_runtime_mutex, runtime_lock_timeout_ticks()) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void unlock_runtime(void)
{
    if (s_runtime_mutex != NULL) {
        xSemaphoreGive(s_runtime_mutex);
    }
}

static esp_err_t lock_transition(void)
{
    if (s_transition_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_transition_mutex, runtime_lock_timeout_ticks()) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void unlock_transition(void)
{
    if (s_transition_mutex != NULL) {
        xSemaphoreGive(s_transition_mutex);
    }
}

static bool requirement_enabled(service_runtime_requirements_t requirements, service_runtime_id_t service)
{
    return ((requirements & (1UL << service)) != 0U);
}

static esp_err_t call_lifecycle_fn(service_lifecycle_fn_t fn, const char *service_name, const char *stage)
{
    if (fn == NULL) {
        return ESP_OK;
    }

    esp_err_t guard_err = service_lifecycle_guard_enter();
    if (guard_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "lifecycle guard denied %s/%s: %s",
                 service_name,
                 stage,
                 esp_err_to_name(guard_err));
        return guard_err;
    }

    esp_err_t err = fn();
    service_lifecycle_guard_exit();
    return err;
}

static esp_err_t ensure_initialized(service_runtime_id_t service)
{
    service_ctx_t *ctx = &s_service_ctx[service];
    const service_desc_t *desc = &s_services[service];

    esp_err_t lock_err = lock_runtime();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    if (ctx->state != SERVICE_RUNTIME_STATE_UNINITIALIZED) {
        unlock_runtime();
        return ESP_OK;
    }
    if (desc->init_fn == NULL) {
        ctx->state = SERVICE_RUNTIME_STATE_STOPPED;
        unlock_runtime();
        return ESP_OK;
    }
    unlock_runtime();

    esp_err_t err = call_lifecycle_fn(desc->init_fn, desc->name, "init");
    lock_err = lock_runtime();
    if (lock_err != ESP_OK) {
        return (err == ESP_OK) ? lock_err : err;
    }
    ctx->state = (err == ESP_OK) ? SERVICE_RUNTIME_STATE_STOPPED : SERVICE_RUNTIME_STATE_ERROR;
    unlock_runtime();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init failed for %s: %s", desc->name, esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t start_service(service_runtime_id_t service)
{
    service_ctx_t *ctx = &s_service_ctx[service];
    const service_desc_t *desc = &s_services[service];

    ESP_RETURN_ON_ERROR(ensure_initialized(service), TAG, "init stage failed");

    esp_err_t lock_err = lock_runtime();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    if (ctx->state == SERVICE_RUNTIME_STATE_RUNNING && !desc->restart_while_running) {
        unlock_runtime();
        return ESP_OK;
    }
    if (desc->start_fn == NULL) {
        ctx->state = SERVICE_RUNTIME_STATE_RUNNING;
        unlock_runtime();
        return ESP_OK;
    }

    ctx->state = SERVICE_RUNTIME_STATE_STARTING;
    unlock_runtime();

    esp_err_t err = call_lifecycle_fn(desc->start_fn, desc->name, "start");
    lock_err = lock_runtime();
    if (lock_err != ESP_OK) {
        return (err == ESP_OK) ? lock_err : err;
    }
    if (err != ESP_OK) {
        ctx->state = SERVICE_RUNTIME_STATE_ERROR;
        unlock_runtime();
        ESP_LOGW(TAG, "start failed for %s: %s", desc->name, esp_err_to_name(err));
        return err;
    }
    ctx->state = SERVICE_RUNTIME_STATE_RUNNING;
    unlock_runtime();

    if (desc->restart_while_running) {
        ESP_LOGI(TAG, "service (re)started: %s", desc->name);
    } else {
        ESP_LOGI(TAG, "service started: %s", desc->name);
    }
    return ESP_OK;
}

static esp_err_t stop_service(service_runtime_id_t service)
{
    service_ctx_t *ctx = &s_service_ctx[service];
    const service_desc_t *desc = &s_services[service];

    esp_err_t lock_err = lock_runtime();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    if (ctx->state != SERVICE_RUNTIME_STATE_RUNNING) {
        unlock_runtime();
        return ESP_OK;
    }
    if (!desc->stop_supported || desc->stop_fn == NULL) {
        unlock_runtime();
        ESP_LOGD(TAG, "service kept running (no stop lifecycle): %s", desc->name);
        return ESP_OK;
    }

    ctx->state = SERVICE_RUNTIME_STATE_STOPPING;
    unlock_runtime();

    esp_err_t err = call_lifecycle_fn(desc->stop_fn, desc->name, "stop");
    lock_err = lock_runtime();
    if (lock_err != ESP_OK) {
        return (err == ESP_OK) ? lock_err : err;
    }
    if (err != ESP_OK) {
        ctx->state = SERVICE_RUNTIME_STATE_ERROR;
        unlock_runtime();
        ESP_LOGW(TAG, "stop failed for %s: %s", desc->name, esp_err_to_name(err));
        return err;
    }
    ctx->state = SERVICE_RUNTIME_STATE_STOPPED;
    unlock_runtime();

    ESP_LOGI(TAG, "service stopped: %s", desc->name);
    return ESP_OK;
}

static void rollback_started_services(service_runtime_requirements_t started_mask)
{
    for (size_t i = sizeof(s_start_order) / sizeof(s_start_order[0]); i > 0; --i) {
        service_runtime_id_t service = s_start_order[i - 1U];
        if ((started_mask & (1UL << service)) == 0U) {
            continue;
        }
        esp_err_t err = stop_service(service);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "rollback: failed to stop %s (%s)",
                     s_services[service].name,
                     esp_err_to_name(err));
        }
    }
}

static service_runtime_requirements_t running_mask_unlocked(void)
{
    service_runtime_requirements_t mask = 0U;
    for (size_t i = 0; i < SERVICE_RUNTIME_COUNT; ++i) {
        if (s_service_ctx[i].state == SERVICE_RUNTIME_STATE_RUNNING) {
            mask |= (1UL << i);
        }
    }
    return mask;
}

static esp_err_t running_mask_snapshot(service_runtime_requirements_t *out_mask)
{
    if (out_mask == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_runtime();
    if (err != ESP_OK) {
        return err;
    }
    *out_mask = running_mask_unlocked();
    unlock_runtime();
    return ESP_OK;
}

static service_runtime_state_t service_state_snapshot(service_runtime_id_t service)
{
    if (service >= SERVICE_RUNTIME_COUNT) {
        return SERVICE_RUNTIME_STATE_ERROR;
    }
    esp_err_t err = lock_runtime();
    if (err != ESP_OK) {
        return SERVICE_RUNTIME_STATE_ERROR;
    }
    service_runtime_state_t state = s_service_ctx[service].state;
    unlock_runtime();
    return state;
}

static esp_err_t apply_requirements(service_runtime_requirements_t requirements)
{
    service_runtime_requirements_t started_now_mask = 0U;

    for (size_t i = 0; i < sizeof(s_start_order) / sizeof(s_start_order[0]); ++i) {
        service_runtime_id_t service = s_start_order[i];
        if (!requirement_enabled(requirements, service)) {
            continue;
        }
        bool was_running = (service_state_snapshot(service) == SERVICE_RUNTIME_STATE_RUNNING);
        esp_err_t err = start_service(service);
        if (err != ESP_OK) {
            if (s_services[service].start_required) {
                rollback_started_services(started_now_mask);
                return err;
            }
            ESP_LOGW(TAG,
                     "non-critical service failed to start: %s (%s), continuing mode switch",
                     s_services[service].name,
                     esp_err_to_name(err));
            continue;
        }
        if (!was_running && service_state_snapshot(service) == SERVICE_RUNTIME_STATE_RUNNING) {
            started_now_mask |= (1UL << service);
        }
    }

    for (size_t i = 0; i < sizeof(s_stop_order) / sizeof(s_stop_order[0]); ++i) {
        service_runtime_id_t service = s_stop_order[i];
        if (requirement_enabled(requirements, service)) {
            continue;
        }
        esp_err_t err = stop_service(service);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "service stop failed for %s (%s), continuing mode switch",
                     s_services[service].name,
                     esp_err_to_name(err));
        }
    }

    service_runtime_requirements_t running_mask = 0U;
    ESP_RETURN_ON_ERROR(running_mask_snapshot(&running_mask), TAG, "running mask snapshot failed");

    ESP_RETURN_ON_ERROR(lock_runtime(), TAG, "runtime lock failed");
    s_active_requirements = running_mask;
    unlock_runtime();

    if (s_active_requirements != requirements) {
        ESP_LOGW(TAG,
                 "runtime mismatch requested=0x%08" PRIx32 " running=0x%08" PRIx32,
                 requirements,
                 s_active_requirements);
    }
    return ESP_OK;
}

static esp_err_t set_network_profile_guarded(network_profile_t profile)
{
    ESP_RETURN_ON_ERROR(service_lifecycle_guard_enter(), TAG, "lifecycle guard enter failed");
    esp_err_t err = network_manager_set_desired_profile(profile);
    service_lifecycle_guard_exit();
    return err;
}

static network_profile_t get_current_desired_profile_snapshot(void)
{
    network_status_t status = { 0 };
    if (network_manager_get_status(&status) == ESP_OK) {
        return status.desired_profile;
    }
    return NETWORK_PROFILE_NONE;
}

esp_err_t service_runtime_init(void)
{
    if (s_runtime_mutex != NULL) {
        return ESP_OK;
    }

    s_runtime_mutex = xSemaphoreCreateMutex();
    if (s_runtime_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_transition_mutex = xSemaphoreCreateMutex();
    if (s_transition_mutex == NULL) {
        vSemaphoreDelete(s_runtime_mutex);
        s_runtime_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < SERVICE_RUNTIME_COUNT; ++i) {
        s_service_ctx[i].state = SERVICE_RUNTIME_STATE_UNINITIALIZED;
    }
    s_active_requirements = 0U;

    ESP_LOGI(TAG, "service runtime initialized");
    return ESP_OK;
}

esp_err_t service_runtime_apply_plan(const service_runtime_mode_plan_t *previous_plan,
                                     const service_runtime_mode_plan_t *target_plan)
{
    ESP_RETURN_ON_ERROR(service_runtime_init(), TAG, "runtime init failed");

    if (target_plan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(lock_transition(), TAG, "runtime transition lock failed");

    service_runtime_requirements_t previous_requirements = 0U;
    esp_err_t mask_err = running_mask_snapshot(&previous_requirements);
    if (mask_err != ESP_OK) {
        unlock_transition();
        return mask_err;
    }

    network_profile_t previous_profile = get_current_desired_profile_snapshot();
    if (previous_profile == NETWORK_PROFILE_NONE && previous_plan != NULL) {
        previous_profile = previous_plan->network_profile;
    }

    /*
     * Transaction contract for mode/runtime/network boundary:
     * 1) set desired network profile,
     * 2) apply service requirements,
     * 3) rollback both profile and requirements on failure.
     */
    esp_err_t set_profile_err = set_network_profile_guarded(target_plan->network_profile);
    if (set_profile_err != ESP_OK) {
        unlock_transition();
        ESP_LOGW(TAG,
                 "network desired profile set failed (target=%s): %s",
                 network_manager_profile_to_str(target_plan->network_profile),
                 esp_err_to_name(set_profile_err));
        return set_profile_err;
    }

    esp_err_t err = apply_requirements(target_plan->requirements);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "apply requirements failed, rolling back to previous mode/runtime: %s",
                 esp_err_to_name(err));
        esp_err_t net_rb_err = set_network_profile_guarded(previous_profile);
        if (net_rb_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "rollback network profile set failed: %s",
                     esp_err_to_name(net_rb_err));
        }
        esp_err_t req_rb_err = apply_requirements(previous_requirements);
        if (req_rb_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "rollback runtime requirements failed: %s",
                     esp_err_to_name(req_rb_err));
        }
        unlock_transition();
        return err;
    }
    unlock_transition();

    ESP_LOGI(TAG, "runtime plan applied: req=0x%08" PRIx32, target_plan->requirements);
    return ESP_OK;
}

service_runtime_state_t service_runtime_get_state(service_runtime_id_t service)
{
    if (service >= SERVICE_RUNTIME_COUNT) {
        return SERVICE_RUNTIME_STATE_ERROR;
    }
    if (s_runtime_mutex == NULL) {
        return SERVICE_RUNTIME_STATE_UNINITIALIZED;
    }
    if (lock_runtime() != ESP_OK) {
        return SERVICE_RUNTIME_STATE_ERROR;
    }
    service_runtime_state_t state = s_service_ctx[service].state;
    unlock_runtime();
    return state;
}

service_runtime_requirements_t service_runtime_get_active_requirements(void)
{
    if (s_runtime_mutex == NULL) {
        return 0U;
    }
    if (lock_runtime() != ESP_OK) {
        return 0U;
    }
    service_runtime_requirements_t requirements = s_active_requirements;
    unlock_runtime();
    return requirements;
}

const char *service_runtime_id_to_str(service_runtime_id_t service)
{
    if (service >= SERVICE_RUNTIME_COUNT) {
        return "unknown";
    }
    return s_services[service].name;
}

const char *service_runtime_state_to_str(service_runtime_state_t state)
{
    switch (state) {
    case SERVICE_RUNTIME_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case SERVICE_RUNTIME_STATE_STOPPED:
        return "STOPPED";
    case SERVICE_RUNTIME_STATE_STARTING:
        return "STARTING";
    case SERVICE_RUNTIME_STATE_RUNNING:
        return "RUNNING";
    case SERVICE_RUNTIME_STATE_STOPPING:
        return "STOPPING";
    case SERVICE_RUNTIME_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
