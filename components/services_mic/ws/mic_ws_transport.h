#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"

TickType_t mic_ws_transport_ms_to_ticks_min1(uint32_t ms);

esp_err_t mic_ws_transport_send_all_bin(esp_websocket_client_handle_t client,
                                        const int16_t *samples,
                                        uint16_t sample_count,
                                        uint32_t timeout_ms,
                                        uint32_t retry_count,
                                        uint32_t retry_backoff_ms,
                                        const char *log_tag);

esp_err_t mic_ws_transport_send_text_retry(esp_websocket_client_handle_t client,
                                           const char *msg,
                                           int len,
                                           uint32_t timeout_ms,
                                           uint32_t retry_count,
                                           uint32_t retry_backoff_ms,
                                           const char *label,
                                           const char *log_tag);
