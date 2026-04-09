#ifndef SERVICE_RUNTIME_H
#define SERVICE_RUNTIME_H

#include <stdint.h>
#include "app_defs.h"
#include "esp_err.h"
#include "network_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVICE_RUNTIME_STATE_UNINITIALIZED = 0,
    SERVICE_RUNTIME_STATE_STOPPED,
    SERVICE_RUNTIME_STATE_STARTING,
    SERVICE_RUNTIME_STATE_RUNNING,
    SERVICE_RUNTIME_STATE_STOPPING,
    SERVICE_RUNTIME_STATE_ERROR,
} service_runtime_state_t;

typedef enum {
    SERVICE_RUNTIME_TOUCH = 0,
    SERVICE_RUNTIME_LED,
    SERVICE_RUNTIME_AUDIO,
    SERVICE_RUNTIME_MIC,
    SERVICE_RUNTIME_AI,
    SERVICE_RUNTIME_STORAGE,
    SERVICE_RUNTIME_NETWORK,
    SERVICE_RUNTIME_MQTT,
    SERVICE_RUNTIME_WEB,
    SERVICE_RUNTIME_OTA,
    SERVICE_RUNTIME_COUNT
} service_runtime_id_t;

typedef uint32_t service_runtime_requirements_t;

#define SERVICE_RUNTIME_REQ_TOUCH (1UL << SERVICE_RUNTIME_TOUCH)
#define SERVICE_RUNTIME_REQ_LED (1UL << SERVICE_RUNTIME_LED)
#define SERVICE_RUNTIME_REQ_AUDIO (1UL << SERVICE_RUNTIME_AUDIO)
#define SERVICE_RUNTIME_REQ_MIC (1UL << SERVICE_RUNTIME_MIC)
#define SERVICE_RUNTIME_REQ_AI (1UL << SERVICE_RUNTIME_AI)
#define SERVICE_RUNTIME_REQ_STORAGE (1UL << SERVICE_RUNTIME_STORAGE)
#define SERVICE_RUNTIME_REQ_NETWORK (1UL << SERVICE_RUNTIME_NETWORK)
#define SERVICE_RUNTIME_REQ_MQTT (1UL << SERVICE_RUNTIME_MQTT)
#define SERVICE_RUNTIME_REQ_WEB (1UL << SERVICE_RUNTIME_WEB)
#define SERVICE_RUNTIME_REQ_OTA (1UL << SERVICE_RUNTIME_OTA)

/*
 * Initializes runtime orchestration state for shared services.
 * Must be called from app_main before app_control_task starts.
 */
esp_err_t service_runtime_init(void);

typedef struct {
    network_profile_t network_profile;
    service_runtime_requirements_t requirements;
} service_runtime_mode_plan_t;

/*
 * Control-context API used by mode/runtime orchestration layer.
 * Applies already resolved runtime plan. service_runtime does not resolve mode policy.
 */
esp_err_t service_runtime_apply_plan(const service_runtime_mode_plan_t *previous_plan,
                                     const service_runtime_mode_plan_t *target_plan);

service_runtime_state_t service_runtime_get_state(service_runtime_id_t service);
service_runtime_requirements_t service_runtime_get_active_requirements(void);

const char *service_runtime_id_to_str(service_runtime_id_t service);
const char *service_runtime_state_to_str(service_runtime_state_t state);

#ifdef __cplusplus
}
#endif

#endif
