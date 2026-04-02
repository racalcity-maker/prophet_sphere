#ifndef AI_TYPES_H
#define AI_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AI_REQUEST_SOURCE_UNKNOWN = 0,
    AI_REQUEST_SOURCE_TOUCH,
    AI_REQUEST_SOURCE_WEB,
    AI_REQUEST_SOURCE_MQTT,
} ai_request_source_t;

typedef struct {
    uint32_t request_id;
    ai_request_source_t source;
} ai_request_meta_t;

#ifdef __cplusplus
}
#endif

#endif
