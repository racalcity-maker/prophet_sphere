#ifndef APP_DEFS_H
#define APP_DEFS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ORB_MODE_NONE = 0,
    ORB_MODE_OFFLINE_SCRIPTED,
    ORB_MODE_HYBRID_AI,
    ORB_MODE_INSTALLATION_SLAVE,
    ORB_MODE_MAX
} orb_mode_t;

typedef enum {
    ORB_INTENT_UNKNOWN = 0,
    ORB_INTENT_LOVE,
    ORB_INTENT_FUTURE,
    ORB_INTENT_CHOICE,
    ORB_INTENT_MONEY,
    ORB_INTENT_PATH,
    ORB_INTENT_DANGER,
    ORB_INTENT_INNER_STATE,
    ORB_INTENT_WISH,
    ORB_INTENT_YES_NO,
    ORB_INTENT_PAST,
    ORB_INTENT_TIME,
    ORB_INTENT_PLACE,
    ORB_INTENT_UNCERTAIN,
    ORB_INTENT_JOKE,
    ORB_INTENT_FORBIDDEN,
    ORB_INTENT_COUNT
} orb_intent_id_t;

#define ORB_INVALID_SESSION_ID UINT32_C(0)

#ifdef __cplusplus
}
#endif

#endif
