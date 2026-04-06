#include "mic_ws_client.h"

#include <stdbool.h>
#include <stdint.h>
#include "esp_check.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "mic_ws_client_internal.h"
#include "mic_ws_protocol.h"
#include "mic_ws_transport.h"

static const char *TAG = LOG_TAG_MIC;

esp_err_t mic_ws_client_session_start(uint32_t capture_id, uint32_t sample_rate_hz)
{
    if (!mic_ws_client_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (capture_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_mic_ws.initialized) {
        ESP_RETURN_ON_ERROR(mic_ws_client_init(), TAG, "mic ws init failed");
    }

    esp_websocket_client_handle_t client = NULL;
    bool reuse_connection = false;
    ESP_RETURN_ON_ERROR(mic_ws_acquire_client(true, &reuse_connection, &client),
                        TAG,
                        "mic ws acquire client failed");

    portENTER_CRITICAL(&g_mic_ws_lock);
    mic_ws_bind_transport_locked(client, reuse_connection);
    mic_ws_prepare_kws_session_locked(capture_id, sample_rate_hz);
    portEXIT_CRITICAL(&g_mic_ws_lock);

    if (!reuse_connection) {
        TickType_t deadline = xTaskGetTickCount() + mic_ws_ms_to_ticks_min1(CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS);
        while (!mic_ws_time_reached(xTaskGetTickCount(), deadline)) {
            if (esp_websocket_client_is_connected(client)) {
                break;
            }
            vTaskDelay(mic_ws_ms_to_ticks_min1(10U));
        }

        if (!esp_websocket_client_is_connected(client)) {
            mic_ws_client_abort();
            return ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t err = mic_ws_send_start_frame(client, capture_id, sample_rate_hz);
    if (err != ESP_OK) {
        mic_ws_client_abort();
        return err;
    }

    portENTER_CRITICAL(&g_mic_ws_lock);
    g_mic_ws.kws.session_active = true;
    g_mic_ws.kws.start_sent = true;
    portEXIT_CRITICAL(&g_mic_ws_lock);
    return ESP_OK;
}

esp_err_t mic_ws_client_session_send_pcm16(const int16_t *samples, uint16_t sample_count)
{
    if (!mic_ws_client_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (samples == NULL || sample_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_websocket_client_handle_t client = NULL;
    bool session_active = false;
    bool connected = false;
    bool start_sent = false;
    uint32_t capture_id = 0U;
    uint32_t sample_rate_hz = 0U;
    portENTER_CRITICAL(&g_mic_ws_lock);
    client = g_mic_ws.transport.client;
    session_active = g_mic_ws.kws.session_active;
    connected = g_mic_ws.transport.connected;
    start_sent = g_mic_ws.kws.start_sent;
    capture_id = g_mic_ws.kws.active_capture_id;
    sample_rate_hz = g_mic_ws.kws.sample_rate_hz;
    portEXIT_CRITICAL(&g_mic_ws_lock);

    if (client == NULL || !session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!connected || !esp_websocket_client_is_connected(client)) {
        if (!mic_ws_wait_connected(client, CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS)) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (!start_sent) {
        esp_err_t start_err = mic_ws_send_start_frame(client, capture_id, sample_rate_hz);
        if (start_err != ESP_OK) {
            return start_err;
        }
        portENTER_CRITICAL(&g_mic_ws_lock);
        g_mic_ws.kws.start_sent = true;
        portEXIT_CRITICAL(&g_mic_ws_lock);
    }

    return mic_ws_transport_send_all_bin(client,
                                         samples,
                                         sample_count,
                                         mic_ws_effective_send_timeout_ms(),
                                         (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_COUNT,
                                         (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_BACKOFF_MS,
                                         TAG);
}

esp_err_t mic_ws_client_session_finish(void)
{
    if (!mic_ws_client_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_websocket_client_handle_t client = NULL;
    uint32_t capture_id = 0U;
    bool session_active = false;
    portENTER_CRITICAL(&g_mic_ws_lock);
    client = g_mic_ws.transport.client;
    capture_id = g_mic_ws.kws.active_capture_id;
    session_active = g_mic_ws.kws.session_active;
    portEXIT_CRITICAL(&g_mic_ws_lock);

    if (client == NULL || !session_active || !esp_websocket_client_is_connected(client)) {
        return ESP_ERR_INVALID_STATE;
    }

    char msg[72];
    ESP_RETURN_ON_ERROR(mic_ws_protocol_build_end_frame(capture_id, msg, sizeof(msg)),
                        TAG,
                        "build end frame failed");
    return mic_ws_transport_send_text_retry(client,
                                            msg,
                                            (int)strlen(msg),
                                            mic_ws_effective_send_timeout_ms(),
                                            (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_COUNT,
                                            (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_BACKOFF_MS,
                                            "end",
                                            TAG);
}

esp_err_t mic_ws_client_take_result(uint32_t capture_id,
                                    orb_intent_id_t *out_intent,
                                    uint16_t *out_confidence_permille,
                                    uint32_t timeout_ms)
{
    if (!mic_ws_client_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (out_intent == NULL || out_confidence_permille == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t deadline = xTaskGetTickCount() + mic_ws_ms_to_ticks_min1(timeout_ms);
    while (!mic_ws_time_reached(xTaskGetTickCount(), deadline)) {
        bool ready = false;
        uint32_t result_capture_id = 0U;
        orb_intent_id_t intent_id = ORB_INTENT_UNKNOWN;
        uint16_t conf = 0U;

        portENTER_CRITICAL(&g_mic_ws_lock);
        if (g_mic_ws.kws.result_ready) {
            ready = true;
            result_capture_id = g_mic_ws.kws.result_capture_id;
            intent_id = g_mic_ws.kws.result_intent;
            conf = g_mic_ws.kws.result_conf_permille;
        }
        portEXIT_CRITICAL(&g_mic_ws_lock);

        if (ready && (result_capture_id == capture_id || result_capture_id == 0U)) {
            *out_intent = intent_id;
            *out_confidence_permille = conf;
            portENTER_CRITICAL(&g_mic_ws_lock);
            g_mic_ws.kws.result_ready = false;
            g_mic_ws.kws.session_active = false;
            g_mic_ws.kws.start_sent = false;
            portEXIT_CRITICAL(&g_mic_ws_lock);
            return ESP_OK;
        }

        vTaskDelay(mic_ws_ms_to_ticks_min1(MIC_WS_RESULT_POLL_MS));
    }

    return ESP_ERR_TIMEOUT;
}
