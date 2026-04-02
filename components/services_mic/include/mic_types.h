#ifndef MIC_TYPES_H
#define MIC_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "app_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;
    uint32_t capture_id;
    uint32_t captured_samples;
    uint16_t level_avg;
    uint16_t level_peak;
    uint16_t intent_confidence_permille;
    orb_intent_id_t intent_id;
} mic_capture_status_t;

#ifdef __cplusplus
}
#endif

#endif
