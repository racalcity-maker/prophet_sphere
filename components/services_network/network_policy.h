#ifndef NETWORK_POLICY_H
#define NETWORK_POLICY_H

#include "network_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

network_profile_t network_policy_resolve_effective_profile(network_profile_t desired_profile);

#ifdef __cplusplus
}
#endif

#endif
