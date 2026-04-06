#include "network_event_handlers.h"

#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static network_event_sink_t s_sink;
static void *s_sink_ctx;

void network_event_handlers_init(const network_event_sink_t *sink, void *ctx)
{
    if (sink == NULL) {
        (void)memset(&s_sink, 0, sizeof(s_sink));
        s_sink_ctx = NULL;
        return;
    }
    s_sink = *sink;
    s_sink_ctx = ctx;
}

void network_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        if (s_sink.on_sta_start != NULL) {
            s_sink.on_sta_start(s_sink_ctx);
        }
        break;
    case WIFI_EVENT_STA_CONNECTED:
        if (s_sink.on_sta_connected != NULL) {
            s_sink.on_sta_connected(s_sink_ctx);
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        uint8_t reason = 0U;
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (disc != NULL) {
            reason = (uint8_t)disc->reason;
        }
        if (s_sink.on_sta_disconnected != NULL) {
            s_sink.on_sta_disconnected(s_sink_ctx, reason);
        }
        break;
    }
    case WIFI_EVENT_AP_START:
        if (s_sink.on_ap_start != NULL) {
            s_sink.on_ap_start(s_sink_ctx);
        }
        break;
    case WIFI_EVENT_AP_STOP:
        if (s_sink.on_ap_stop != NULL) {
            s_sink.on_ap_stop(s_sink_ctx);
        }
        break;
    default:
        break;
    }
}

void network_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    if (event_id != IP_EVENT_STA_GOT_IP || s_sink.on_sta_got_ip == NULL) {
        return;
    }

    ip_event_got_ip_t *ip_event = (ip_event_got_ip_t *)event_data;
    if (ip_event == NULL) {
        s_sink.on_sta_got_ip(s_sink_ctx, NULL);
        return;
    }
    s_sink.on_sta_got_ip(s_sink_ctx, &ip_event->ip_info.ip);
}
