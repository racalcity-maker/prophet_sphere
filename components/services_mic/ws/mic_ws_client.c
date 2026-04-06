#include "mic_ws_client.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "mic_ws_client_internal.h"
#include "mic_ws_protocol.h"
#include "mic_ws_tts_stream.h"
#include "mic_ws_transport.h"

static const char *TAG = LOG_TAG_MIC;

mic_ws_state_t g_mic_ws;
portMUX_TYPE g_mic_ws_lock = portMUX_INITIALIZER_UNLOCKED;

/* The following helpers must be called with g_mic_ws_lock held. */
static void ws_reset_transport_locked(void)
{
    g_mic_ws.transport.client = NULL;
    g_mic_ws.transport.connected = false;
    g_mic_ws.transport.rx_len = 0U;
}

static void ws_reset_kws_locked(void)
{
    g_mic_ws.kws.session_active = false;
    g_mic_ws.kws.start_sent = false;
    g_mic_ws.kws.active_capture_id = 0U;
    g_mic_ws.kws.sample_rate_hz = 0U;
    g_mic_ws.kws.result_ready = false;
    g_mic_ws.kws.result_capture_id = 0U;
    g_mic_ws.kws.result_intent = ORB_INTENT_UNKNOWN;
    g_mic_ws.kws.result_conf_permille = 0U;
}

static void ws_reset_tts_locked(void)
{
    g_mic_ws.tts.tts_active = false;
    g_mic_ws.tts.tts_done = false;
    g_mic_ws.tts.tts_failed = false;
    g_mic_ws.tts.tts_chunk_cb = NULL;
    g_mic_ws.tts.tts_chunk_cb_ctx = NULL;
    g_mic_ws.tts.tts_chunks_sent = 0U;
    g_mic_ws.tts.tts_chunks_dropped = 0U;
    g_mic_ws.tts.tts_bytes_rx = 0U;
    g_mic_ws.tts.tts_frames_rx = 0U;
    g_mic_ws.tts.tts_boundary_jump_count = 0U;
    g_mic_ws.tts.tts_boundary_jump_max = 0U;
    g_mic_ws.tts.tts_prev_sample_valid = false;
    g_mic_ws.tts.tts_prev_sample = 0;
    g_mic_ws.tts.tts_started_tick = 0U;
    g_mic_ws.tts.tts_last_diag_tick = 0U;
    g_mic_ws.tts.tts_pcm_tail_count = 0U;
    g_mic_ws.tts.sample_rate_hz = 0U;
}

TickType_t mic_ws_ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

bool mic_ws_time_reached(TickType_t now, TickType_t deadline)
{
    return ((int32_t)(now - deadline) >= 0);
}

uint32_t mic_ws_effective_send_timeout_ms(void)
{
    if ((uint32_t)CONFIG_ORB_MIC_WS_SEND_TIMEOUT_MS < MIC_WS_SEND_TIMEOUT_MIN_MS) {
        return MIC_WS_SEND_TIMEOUT_MIN_MS;
    }
    return (uint32_t)CONFIG_ORB_MIC_WS_SEND_TIMEOUT_MS;
}

bool mic_ws_wait_connected(esp_websocket_client_handle_t client, uint32_t timeout_ms)
{
    if (client == NULL) {
        return false;
    }
    TickType_t deadline = xTaskGetTickCount() + mic_ws_ms_to_ticks_min1(timeout_ms);
    while (!mic_ws_time_reached(xTaskGetTickCount(), deadline)) {
        bool connected = false;
        portENTER_CRITICAL(&g_mic_ws_lock);
        connected = g_mic_ws.transport.connected;
        portEXIT_CRITICAL(&g_mic_ws_lock);
        if (connected && esp_websocket_client_is_connected(client)) {
            return true;
        }
        vTaskDelay(mic_ws_ms_to_ticks_min1(10U));
    }
    return false;
}

esp_err_t mic_ws_send_start_frame(esp_websocket_client_handle_t client, uint32_t capture_id, uint32_t sample_rate_hz)
{
    char msg[384];
    ESP_RETURN_ON_ERROR(mic_ws_protocol_build_start_frame(capture_id, sample_rate_hz, msg, sizeof(msg)),
                        TAG,
                        "build start frame failed");
    return mic_ws_transport_send_text_retry(client,
                                            msg,
                                            (int)strlen(msg),
                                            mic_ws_effective_send_timeout_ms(),
                                            (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_COUNT,
                                            (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_BACKOFF_MS,
                                            "start",
                                            TAG);
}

esp_err_t mic_ws_send_tts_request(esp_websocket_client_handle_t client, const char *text, uint32_t sample_rate_hz)
{
    if (client == NULL || text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char msg[1200];
    ESP_RETURN_ON_ERROR(mic_ws_protocol_build_tts_request(text,
                                                          sample_rate_hz,
                                                          CONFIG_ORB_MIC_WS_TTS_VOICE,
                                                          msg,
                                                          sizeof(msg)),
                        TAG,
                        "build tts request failed");
    return mic_ws_transport_send_text_retry(client,
                                            msg,
                                            (int)strlen(msg),
                                            mic_ws_effective_send_timeout_ms(),
                                            (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_COUNT,
                                            (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_BACKOFF_MS,
                                            "tts",
                                            TAG);
}

void mic_ws_prepare_kws_session_locked(uint32_t capture_id, uint32_t sample_rate_hz)
{
    g_mic_ws.transport.rx_len = 0U;
    ws_reset_kws_locked();
    ws_reset_tts_locked();
    g_mic_ws.kws.active_capture_id = capture_id;
    g_mic_ws.kws.sample_rate_hz = sample_rate_hz;
    g_mic_ws.mode = MIC_WS_MODE_KWS;
}

void mic_ws_prepare_tts_session_locked(uint32_t sample_rate_hz,
                                       mic_ws_tts_chunk_cb_t chunk_cb,
                                       void *chunk_cb_ctx)
{
    g_mic_ws.transport.rx_len = 0U;
    ws_reset_kws_locked();
    ws_reset_tts_locked();
    g_mic_ws.mode = MIC_WS_MODE_TTS;
    g_mic_ws.tts.tts_active = true;
    g_mic_ws.tts.tts_done = false;
    g_mic_ws.tts.tts_failed = false;
    g_mic_ws.tts.tts_chunk_cb = chunk_cb;
    g_mic_ws.tts.tts_chunk_cb_ctx = chunk_cb_ctx;
    g_mic_ws.tts.tts_chunks_sent = 0U;
    g_mic_ws.tts.tts_chunks_dropped = 0U;
    g_mic_ws.tts.tts_bytes_rx = 0U;
    g_mic_ws.tts.tts_frames_rx = 0U;
    g_mic_ws.tts.tts_boundary_jump_count = 0U;
    g_mic_ws.tts.tts_boundary_jump_max = 0U;
    g_mic_ws.tts.tts_prev_sample_valid = false;
    g_mic_ws.tts.tts_prev_sample = 0;
    g_mic_ws.tts.tts_started_tick = xTaskGetTickCount();
    g_mic_ws.tts.tts_last_diag_tick = 0U;
    g_mic_ws.tts.tts_pcm_tail_count = 0U;
    g_mic_ws.tts.sample_rate_hz = sample_rate_hz;
}

static bool ws_can_reuse_connection_locked(bool block_if_tts_active)
{
    if (g_mic_ws.transport.client == NULL || !g_mic_ws.transport.connected) {
        return false;
    }
    if (block_if_tts_active && g_mic_ws.tts.tts_active) {
        return false;
    }
    return esp_websocket_client_is_connected(g_mic_ws.transport.client);
}

static esp_err_t ws_create_and_start_client(esp_websocket_client_handle_t *out_client)
{
    if (out_client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_websocket_client_config_t cfg = { 0 };
    cfg.uri = CONFIG_ORB_MIC_WS_URL;
    cfg.network_timeout_ms = CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS;
    cfg.reconnect_timeout_ms = CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS;
    cfg.disable_auto_reconnect = true;
    cfg.buffer_size = 4096;
    cfg.task_stack = MIC_WS_CLIENT_TASK_STACK_BYTES;

    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, mic_ws_websocket_event_handler, NULL);
    if (err != ESP_OK) {
        (void)esp_websocket_client_destroy(client);
        return err;
    }

    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        (void)esp_websocket_client_destroy(client);
        return err;
    }

    *out_client = client;
    return ESP_OK;
}

esp_err_t mic_ws_acquire_client(bool block_if_tts_active,
                                bool *out_reuse_connection,
                                esp_websocket_client_handle_t *out_client)
{
    if (out_reuse_connection == NULL || out_client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool reuse_connection = false;
    esp_websocket_client_handle_t client = NULL;
    portENTER_CRITICAL(&g_mic_ws_lock);
    if (ws_can_reuse_connection_locked(block_if_tts_active)) {
        client = g_mic_ws.transport.client;
        reuse_connection = true;
    }
    portEXIT_CRITICAL(&g_mic_ws_lock);

    if (!reuse_connection) {
        mic_ws_client_abort();
        ESP_RETURN_ON_ERROR(ws_create_and_start_client(&client), TAG, "mic ws start failed");
    }

    *out_reuse_connection = reuse_connection;
    *out_client = client;
    return ESP_OK;
}

void mic_ws_bind_transport_locked(esp_websocket_client_handle_t client, bool connected)
{
    g_mic_ws.transport.client = client;
    g_mic_ws.transport.connected = connected;
    g_mic_ws.transport.rx_len = 0U;
}

void mic_ws_websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        portENTER_CRITICAL(&g_mic_ws_lock);
        g_mic_ws.transport.connected = true;
        if (g_mic_ws.mode == MIC_WS_MODE_KWS) {
            g_mic_ws.kws.start_sent = false;
        }
        portEXIT_CRITICAL(&g_mic_ws_lock);
        return;
    }

    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        mic_ws_tts_stream_flush_tail(&g_mic_ws, &g_mic_ws_lock);
        bool was_tts = false;
        bool tts_active = false;
        bool tts_done = false;
        portENTER_CRITICAL(&g_mic_ws_lock);
        was_tts = (g_mic_ws.mode == MIC_WS_MODE_TTS);
        tts_active = g_mic_ws.tts.tts_active;
        tts_done = g_mic_ws.tts.tts_done;
        g_mic_ws.transport.connected = false;
        g_mic_ws.transport.rx_len = 0U;
        if (g_mic_ws.mode == MIC_WS_MODE_KWS) {
            g_mic_ws.kws.start_sent = false;
        }
        g_mic_ws.tts.tts_pcm_tail_count = 0U;
        if (g_mic_ws.mode == MIC_WS_MODE_TTS && g_mic_ws.tts.tts_active && !g_mic_ws.tts.tts_done) {
            g_mic_ws.tts.tts_failed = true;
            g_mic_ws.tts.tts_active = false;
        }
        portEXIT_CRITICAL(&g_mic_ws_lock);
        if (was_tts && tts_active && !tts_done) {
            ESP_LOGW(TAG, "mic ws tts disconnected before done");
        }
        return;
    }

    if (event_id != WEBSOCKET_EVENT_DATA || event_data == NULL) {
        return;
    }

    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;
    if (ev->data_ptr == NULL || ev->data_len <= 0) {
        return;
    }

    char msg[WS_RX_BUF_BYTES];
    bool message_ready = false;
    uint32_t active_capture_id = 0U;
    mic_ws_mode_t mode = MIC_WS_MODE_NONE;
    int op_code = ev->op_code;

    portENTER_CRITICAL(&g_mic_ws_lock);
    mode = g_mic_ws.mode;
    if (ev->payload_offset == 0) {
        g_mic_ws.transport.rx_len = 0U;
    }
    if (!(mode == MIC_WS_MODE_TTS && op_code != 0x1)) {
        size_t room = sizeof(g_mic_ws.transport.rx_buf) - 1U - g_mic_ws.transport.rx_len;
        size_t copy = (size_t)ev->data_len;
        if (copy > room) {
            copy = room;
        }
        if (copy > 0U) {
            memcpy(&g_mic_ws.transport.rx_buf[g_mic_ws.transport.rx_len], ev->data_ptr, copy);
            g_mic_ws.transport.rx_len += copy;
        }
        g_mic_ws.transport.rx_buf[g_mic_ws.transport.rx_len] = '\0';
    }
    active_capture_id = g_mic_ws.kws.active_capture_id;

    if ((ev->payload_offset + ev->data_len) >= ev->payload_len &&
        !(mode == MIC_WS_MODE_TTS && op_code != 0x1)) {
        memcpy(msg, g_mic_ws.transport.rx_buf, g_mic_ws.transport.rx_len + 1U);
        g_mic_ws.transport.rx_len = 0U;
        message_ready = true;
    }
    portEXIT_CRITICAL(&g_mic_ws_lock);

    if (mode == MIC_WS_MODE_TTS && op_code != 0x1) {
        mic_ws_tts_stream_handle_binary(&g_mic_ws, &g_mic_ws_lock, (const uint8_t *)ev->data_ptr, (size_t)ev->data_len, TAG);
        return;
    }

    if (!message_ready) {
        return;
    }

    if (CONFIG_ORB_MIC_WS_LOG_RESULTS) {
        ESP_LOGD(TAG, "mic ws rx: %s", msg);
    }

    if (mode == MIC_WS_MODE_TTS) {
        bool done = false;
        bool failed = false;
        if (mic_ws_protocol_parse_tts_control_message(msg, &done, &failed)) {
            if (done) {
                mic_ws_tts_stream_flush_tail(&g_mic_ws, &g_mic_ws_lock);
            }
            portENTER_CRITICAL(&g_mic_ws_lock);
            if (done) {
                g_mic_ws.tts.tts_done = true;
                g_mic_ws.tts.tts_active = false;
            }
            if (failed) {
                g_mic_ws.tts.tts_failed = true;
                g_mic_ws.tts.tts_active = false;
            }
            portEXIT_CRITICAL(&g_mic_ws_lock);
            if (failed) {
                ESP_LOGW(TAG, "mic ws tts control error: %s", msg);
            }
            if (done) {
                ESP_LOGI(TAG, "mic ws tts control done");
            }
        }
        return;
    }

    uint32_t result_capture_id = active_capture_id;
    orb_intent_id_t intent_id = ORB_INTENT_UNKNOWN;
    uint16_t conf_permille = 0U;
    if (!mic_ws_protocol_parse_result_message(msg, active_capture_id, &result_capture_id, &intent_id, &conf_permille)) {
        return;
    }

    portENTER_CRITICAL(&g_mic_ws_lock);
    g_mic_ws.kws.result_capture_id = result_capture_id;
    g_mic_ws.kws.result_intent = intent_id;
    g_mic_ws.kws.result_conf_permille = conf_permille;
    g_mic_ws.kws.result_ready = true;
    portEXIT_CRITICAL(&g_mic_ws_lock);

    ESP_LOGI(TAG,
             "mic ws result capture=%" PRIu32 " intent=%s conf=%u",
             result_capture_id,
             orb_intent_name(intent_id),
             (unsigned)conf_permille);
}

bool mic_ws_client_is_enabled(void)
{
    return (CONFIG_ORB_MIC_WS_ENABLE && (CONFIG_ORB_MIC_WS_URL[0] != '\0'));
}

esp_err_t mic_ws_client_init(void)
{
    if (!mic_ws_client_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    portENTER_CRITICAL(&g_mic_ws_lock);
    if (g_mic_ws.initialized) {
        portEXIT_CRITICAL(&g_mic_ws_lock);
        return ESP_OK;
    }
    memset(&g_mic_ws, 0, sizeof(g_mic_ws));
    g_mic_ws.initialized = true;
    portEXIT_CRITICAL(&g_mic_ws_lock);

    ESP_LOGI(TAG, "mic ws enabled url=%s", CONFIG_ORB_MIC_WS_URL);
    return ESP_OK;
}

void mic_ws_client_abort(void)
{
    esp_websocket_client_handle_t client = NULL;

    portENTER_CRITICAL(&g_mic_ws_lock);
    client = g_mic_ws.transport.client;
    ws_reset_transport_locked();
    ws_reset_kws_locked();
    ws_reset_tts_locked();
    g_mic_ws.mode = MIC_WS_MODE_NONE;
    portEXIT_CRITICAL(&g_mic_ws_lock);

    if (client != NULL) {
        (void)esp_websocket_client_destroy_on_exit(client);
        esp_err_t stop_err = esp_websocket_client_stop(client);
        if (stop_err != ESP_OK) {
            (void)esp_websocket_client_destroy(client);
        }
    }
}

void mic_ws_client_deinit(void)
{
    mic_ws_client_abort();
    portENTER_CRITICAL(&g_mic_ws_lock);
    g_mic_ws.initialized = false;
    portEXIT_CRITICAL(&g_mic_ws_lock);
}
