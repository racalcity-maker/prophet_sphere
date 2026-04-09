#include "orb_mode_runtime_policy.h"

#include <stdbool.h>
#include "sdkconfig.h"
#include "service_runtime.h"

#define ORB_MODE_ALWAYS_ON_REQUIREMENTS \
    (SERVICE_RUNTIME_REQ_TOUCH | SERVICE_RUNTIME_REQ_LED | SERVICE_RUNTIME_REQ_AUDIO)

static network_profile_t orb_mode_network_profile(orb_mode_t mode)
{
#if !CONFIG_ORB_ENABLE_NETWORK
    (void)mode;
    return NETWORK_PROFILE_NONE;
#else
    network_status_t network_status = { 0 };
    const bool has_network_status = (network_manager_get_status(&network_status) == ESP_OK);
    const bool sta_link_is_up = has_network_status && network_status.network_up &&
                                (network_status.active_profile == NETWORK_PROFILE_STA ||
                                 network_status.active_profile == NETWORK_PROFILE_APSTA);

    switch (mode) {
    case ORB_MODE_OFFLINE_SCRIPTED:
#if CONFIG_ORB_NETWORK_OFFLINE_USE_SOFTAP
        if (sta_link_is_up) {
            return NETWORK_PROFILE_STA;
        }
        return NETWORK_PROFILE_SOFTAP;
#else
        return NETWORK_PROFILE_NONE;
#endif
    case ORB_MODE_HYBRID_AI:
#if CONFIG_ORB_NETWORK_HYBRID_USE_STA
        return NETWORK_PROFILE_APSTA;
#else
        return NETWORK_PROFILE_NONE;
#endif
    case ORB_MODE_INSTALLATION_SLAVE:
#if CONFIG_ORB_NETWORK_INSTALLATION_USE_STA
        return NETWORK_PROFILE_APSTA;
#elif CONFIG_ORB_NETWORK_INSTALLATION_USE_SOFTAP
        return NETWORK_PROFILE_SOFTAP;
#else
        return NETWORK_PROFILE_NONE;
#endif
    case ORB_MODE_NONE:
    default:
        return NETWORK_PROFILE_NONE;
    }
#endif
}

static service_runtime_requirements_t orb_mode_requirements(orb_mode_t mode, network_profile_t net_profile)
{
    const bool want_network = (net_profile != NETWORK_PROFILE_NONE);
    service_runtime_requirements_t net_group = 0U;
    if (want_network) {
        net_group |= SERVICE_RUNTIME_REQ_NETWORK;
        net_group |= SERVICE_RUNTIME_REQ_WEB;
    }

    switch (mode) {
    case ORB_MODE_HYBRID_AI:
        if (want_network) {
            return ORB_MODE_ALWAYS_ON_REQUIREMENTS | SERVICE_RUNTIME_REQ_MIC | SERVICE_RUNTIME_REQ_STORAGE | net_group;
        }
        return ORB_MODE_ALWAYS_ON_REQUIREMENTS | SERVICE_RUNTIME_REQ_MIC | SERVICE_RUNTIME_REQ_STORAGE;
    case ORB_MODE_INSTALLATION_SLAVE:
        return ORB_MODE_ALWAYS_ON_REQUIREMENTS | SERVICE_RUNTIME_REQ_MIC | SERVICE_RUNTIME_REQ_STORAGE | net_group;
    case ORB_MODE_OFFLINE_SCRIPTED:
        return ORB_MODE_ALWAYS_ON_REQUIREMENTS | SERVICE_RUNTIME_REQ_MIC | SERVICE_RUNTIME_REQ_STORAGE | net_group;
    case ORB_MODE_NONE:
    default:
        return ORB_MODE_ALWAYS_ON_REQUIREMENTS;
    }
}

static void orb_mode_build_runtime_plan(orb_mode_t mode, service_runtime_mode_plan_t *out_plan)
{
    out_plan->network_profile = orb_mode_network_profile(mode);
    out_plan->requirements = orb_mode_requirements(mode, out_plan->network_profile);
}

esp_err_t orb_mode_runtime_apply(orb_mode_t previous_mode, orb_mode_t target_mode)
{
    service_runtime_mode_plan_t previous_plan = { 0 };
    service_runtime_mode_plan_t target_plan = { 0 };
    orb_mode_build_runtime_plan(previous_mode, &previous_plan);
    orb_mode_build_runtime_plan(target_mode, &target_plan);
    return service_runtime_apply_plan(&previous_plan, &target_plan);
}
