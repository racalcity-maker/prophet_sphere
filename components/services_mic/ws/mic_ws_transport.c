#include "mic_ws_transport.h"

#include <inttypes.h>
#include <stddef.h>
#include "esp_log.h"
#include "freertos/task.h"

TickType_t mic_ws_transport_ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

esp_err_t mic_ws_transport_send_all_bin(esp_websocket_client_handle_t client,
                                        const int16_t *samples,
                                        uint16_t sample_count,
                                        uint32_t timeout_ms,
                                        uint32_t retry_count,
                                        uint32_t retry_backoff_ms,
                                        const char *log_tag)
{
    if (client == NULL || samples == NULL || sample_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *ptr = (const char *)samples;
    size_t left = (size_t)sample_count * sizeof(int16_t);
    uint32_t attempt = 0U;

    while (left > 0U) {
        int sent = esp_websocket_client_send_bin(client,
                                                 ptr,
                                                 left,
                                                 mic_ws_transport_ms_to_ticks_min1(timeout_ms));
        if (sent > 0) {
            ptr += (size_t)sent;
            left -= (size_t)sent;
            attempt = 0U;
            continue;
        }

        if (attempt >= retry_count) {
            ESP_LOGW(log_tag, "mic ws send bin failed after retries (left=%u)", (unsigned)left);
            return (sent == 0) ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }

        attempt++;
        vTaskDelay(mic_ws_transport_ms_to_ticks_min1(retry_backoff_ms));
    }

    return ESP_OK;
}

esp_err_t mic_ws_transport_send_text_retry(esp_websocket_client_handle_t client,
                                           const char *msg,
                                           int len,
                                           uint32_t timeout_ms,
                                           uint32_t retry_count,
                                           uint32_t retry_backoff_ms,
                                           const char *label,
                                           const char *log_tag)
{
    if (client == NULL || msg == NULL || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t attempt = 0U; attempt <= retry_count; ++attempt) {
        int sent = esp_websocket_client_send_text(client, msg, len, mic_ws_transport_ms_to_ticks_min1(timeout_ms));
        if (sent == len) {
            return ESP_OK;
        }
        if (sent > 0) {
            ESP_LOGW(log_tag, "mic ws %s partial send %d/%d (attempt=%u)", label, sent, len, (unsigned)attempt + 1U);
        }
        if (attempt < retry_count) {
            vTaskDelay(mic_ws_transport_ms_to_ticks_min1(retry_backoff_ms));
        }
    }

    ESP_LOGW(log_tag, "mic ws %s send failed after retries", label);
    return ESP_ERR_TIMEOUT;
}
