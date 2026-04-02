#ifndef ORB_INTENTS_H
#define ORB_INTENTS_H

#include "app_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *orb_intent_name(orb_intent_id_t id);
orb_intent_id_t orb_intent_from_string(const char *intent_str);

#ifdef __cplusplus
}
#endif

#endif
