#ifndef NETWORK_EVENT_HANDLERS_H
#define NETWORK_EVENT_HANDLERS_H

#include <stdint.h>
#include "esp_event.h"
#include "esp_netif_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_sta_start)(void *ctx);
    void (*on_sta_connected)(void *ctx);
    void (*on_sta_disconnected)(void *ctx, uint8_t reason);
    void (*on_ap_start)(void *ctx);
    void (*on_ap_stop)(void *ctx);
    void (*on_sta_got_ip)(void *ctx, const esp_ip4_addr_t *ip);
} network_event_sink_t;

void network_event_handlers_init(const network_event_sink_t *sink, void *ctx);
void network_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void network_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

#ifdef __cplusplus
}
#endif

#endif
