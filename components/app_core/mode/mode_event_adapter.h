#ifndef MODE_EVENT_ADAPTER_H
#define MODE_EVENT_ADAPTER_H

#include "app_events.h"
#include "app_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

app_mode_event_id_t mode_event_adapter_map_id(app_event_id_t id);
void mode_event_adapter_from_app_event(const app_event_t *event, app_mode_event_t *mode_event);

#ifdef __cplusplus
}
#endif

#endif

