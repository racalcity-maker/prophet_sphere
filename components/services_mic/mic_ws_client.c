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
#include "mic_ws_client_state.h"
#include "mic_ws_protocol.h"
#include "mic_ws_tts_stream.h"
#include "mic_ws_transport.h"
#include "orb_intents.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_MIC_WS_ENABLE
#define CONFIG_ORB_MIC_WS_ENABLE 0
#endif
#ifndef CONFIG_ORB_MIC_WS_URL
#define CONFIG_ORB_MIC_WS_URL ""
#endif
#ifndef CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS
#define CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS 1500
#endif
#ifndef CONFIG_ORB_MIC_WS_SEND_TIMEOUT_MS
#define CONFIG_ORB_MIC_WS_SEND_TIMEOUT_MS 200
#endif
#ifndef CONFIG_ORB_MIC_WS_LOG_RESULTS
#define CONFIG_ORB_MIC_WS_LOG_RESULTS 0
#endif
#ifndef CONFIG_ORB_MIC_WS_SEND_RETRY_COUNT
#define CONFIG_ORB_MIC_WS_SEND_RETRY_COUNT 6
#endif
#ifndef CONFIG_ORB_MIC_WS_SEND_RETRY_BACKOFF_MS
#define CONFIG_ORB_MIC_WS_SEND_RETRY_BACKOFF_MS 20
#endif
#ifndef CONFIG_ORB_MIC_WS_TTS_TIMEOUT_MS
#define CONFIG_ORB_MIC_WS_TTS_TIMEOUT_MS 15000
#endif
#ifndef CONFIG_ORB_MIC_WS_TTS_FIRST_CHUNK_TIMEOUT_MS
#define CONFIG_ORB_MIC_WS_TTS_FIRST_CHUNK_TIMEOUT_MS 7000
#endif
#ifndef CONFIG_ORB_MIC_WS_TTS_VOICE
#define CONFIG_ORB_MIC_WS_TTS_VOICE "ruslan"
#endif

#define WS_RESULT_POLL_MS 10U
#define WS_SEND_TIMEOUT_MIN_MS 120U
#define WS_CLIENT_TASK_STACK_BYTES 12288

static const char *TAG = LOG_TAG_MIC;

static mic_ws_state_t s_ws;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool time_reached(TickType_t now, TickType_t deadline);

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static uint32_t ws_effective_send_timeout_ms(void)
{
    if ((uint32_t)CONFIG_ORB_MIC_WS_SEND_TIMEOUT_MS < WS_SEND_TIMEOUT_MIN_MS) {
        return WS_SEND_TIMEOUT_MIN_MS;
    }
    return (uint32_t)CONFIG_ORB_MIC_WS_SEND_TIMEOUT_MS;
}

static bool ws_wait_connected(esp_websocket_client_handle_t client, uint32_t timeout_ms)
{
    if (client == NULL) {
        return false;
    }
    TickType_t deadline = xTaskGetTickCount() + ms_to_ticks_min1(timeout_ms);
    while (!time_reached(xTaskGetTickCount(), deadline)) {
        bool connected = false;
        portENTER_CRITICAL(&s_lock);
        connected = s_ws.connected;
        portEXIT_CRITICAL(&s_lock);
        if (connected && esp_websocket_client_is_connected(client)) {
            return true;
        }
        vTaskDelay(ms_to_ticks_min1(10U));
    }
    return false;
}

static bool time_reached(TickType_t now, TickType_t deadline)
{
    return ((int32_t)(now - deadline) >= 0);
}

static esp_err_t ws_send_start_frame(esp_websocket_client_handle_t client, uint32_t capture_id, uint32_t sample_rate_hz)
{
    char msg[384];
    ESP_RETURN_ON_ERROR(mic_ws_protocol_build_start_frame(capture_id, sample_rate_hz, msg, sizeof(msg)),
                        TAG,
                        "build start frame failed");
    return mic_ws_transport_send_text_retry(client,
                                            msg,
                                            (int)strlen(msg),
                                            ws_effective_send_timeout_ms(),
                                            (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_COUNT,
                                            (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_BACKOFF_MS,
                                            "start",
                                            TAG);
}

static esp_err_t ws_send_tts_request(esp_websocket_client_handle_t client, const char *text, uint32_t sample_rate_hz)
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
                                            ws_effective_send_timeout_ms(),
                                            (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_COUNT,
                                            (uint32_t)CONFIG_ORB_MIC_WS_SEND_RETRY_BACKOFF_MS,
                                            "tts",
                                            TAG);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        portENTER_CRITICAL(&s_lock);
        s_ws.connected = true;
        s_ws.start_sent = false;
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        mic_ws_tts_stream_flush_tail(&s_ws, &s_lock);
        bool was_tts = false;
        bool tts_active = false;
        bool tts_done = false;
        portENTER_CRITICAL(&s_lock);
        was_tts = (s_ws.mode == MIC_WS_MODE_TTS);
        tts_active = s_ws.tts_active;
        tts_done = s_ws.tts_done;
        s_ws.connected = false;
        s_ws.start_sent = false;
        s_ws.rx_len = 0U;
        s_ws.tts_pcm_tail_count = 0U;
        if (s_ws.mode == MIC_WS_MODE_TTS && s_ws.tts_active && !s_ws.tts_done) {
            s_ws.tts_failed = true;
            s_ws.tts_active = false;
        }
        portEXIT_CRITICAL(&s_lock);
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

    portENTER_CRITICAL(&s_lock);
    mode = s_ws.mode;
    if (ev->payload_offset == 0) {
        s_ws.rx_len = 0U;
    }
    if (!(mode == MIC_WS_MODE_TTS && op_code != 0x1)) {
        size_t room = sizeof(s_ws.rx_buf) - 1U - s_ws.rx_len;
        size_t copy = (size_t)ev->data_len;
        if (copy > room) {
            copy = room;
        }
        if (copy > 0U) {
            memcpy(&s_ws.rx_buf[s_ws.rx_len], ev->data_ptr, copy);
            s_ws.rx_len += copy;
        }
        s_ws.rx_buf[s_ws.rx_len] = '\0';
    }
    active_capture_id = s_ws.active_capture_id;

    if ((ev->payload_offset + ev->data_len) >= ev->payload_len &&
        !(mode == MIC_WS_MODE_TTS && op_code != 0x1)) {
        memcpy(msg, s_ws.rx_buf, s_ws.rx_len + 1U);
        s_ws.rx_len = 0U;
        message_ready = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (mode == MIC_WS_MODE_TTS && op_code != 0x1) {
        mic_ws_tts_stream_handle_binary(&s_ws, &s_lock, (const uint8_t *)ev->data_ptr, (size_t)ev->data_len, TAG);
        return;
    }

    if (!message_ready) {
        return;
    }

    if (CONFIG_ORB_MIC_WS_LOG_RESULTS) {
        ESP_LOGI(TAG, "mic ws rx: %s", msg);
    }

    if (mode == MIC_WS_MODE_TTS) {
        bool done = false;
        bool failed = false;
        if (mic_ws_protocol_parse_tts_control_message(msg, &done, &failed)) {
            if (done) {
                mic_ws_tts_stream_flush_tail(&s_ws, &s_lock);
            }
            portENTER_CRITICAL(&s_lock);
            if (done) {
                s_ws.tts_done = true;
                s_ws.tts_active = false;
            }
            if (failed) {
                s_ws.tts_failed = true;
                s_ws.tts_active = false;
            }
            portEXIT_CRITICAL(&s_lock);
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

    portENTER_CRITICAL(&s_lock);
    s_ws.result_capture_id = result_capture_id;
    s_ws.result_intent = intent_id;
    s_ws.result_conf_permille = conf_permille;
    s_ws.result_ready = true;
    portEXIT_CRITICAL(&s_lock);

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

    portENTER_CRITICAL(&s_lock);
    if (s_ws.initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    memset(&s_ws, 0, sizeof(s_ws));
    s_ws.initialized = true;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "mic ws enabled url=%s", CONFIG_ORB_MIC_WS_URL);
    return ESP_OK;
}

void mic_ws_client_abort(void)
{
    esp_websocket_client_handle_t client = NULL;

    portENTER_CRITICAL(&s_lock);
    client = s_ws.client;
    s_ws.client = NULL;
    s_ws.connected = false;
    s_ws.session_active = false;
    s_ws.start_sent = false;
    s_ws.active_capture_id = 0U;
    s_ws.sample_rate_hz = 0U;
    s_ws.rx_len = 0U;
    s_ws.mode = MIC_WS_MODE_NONE;
    s_ws.tts_active = false;
    s_ws.tts_done = false;
    s_ws.tts_failed = false;
    s_ws.tts_chunk_cb = NULL;
    s_ws.tts_chunk_cb_ctx = NULL;
    s_ws.tts_chunks_sent = 0U;
    s_ws.tts_chunks_dropped = 0U;
    s_ws.tts_bytes_rx = 0U;
    s_ws.tts_frames_rx = 0U;
    s_ws.tts_started_tick = 0U;
    s_ws.tts_last_diag_tick = 0U;
    s_ws.tts_pcm_tail_count = 0U;
    portEXIT_CRITICAL(&s_lock);

    if (client != NULL) {
        /* Avoid double-free race with websocket task teardown:
         * if running, let websocket task destroy its own resources on exit. */
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
    portENTER_CRITICAL(&s_lock);
    s_ws.initialized = false;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t mic_ws_client_session_start(uint32_t capture_id, uint32_t sample_rate_hz)
{
    if (!mic_ws_client_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (capture_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ws.initialized) {
        ESP_RETURN_ON_ERROR(mic_ws_client_init(), TAG, "mic ws init failed");
    }

    mic_ws_client_abort();

    esp_websocket_client_config_t cfg = { 0 };
        cfg.uri = CONFIG_ORB_MIC_WS_URL;
        /* Connection/handshake path must use connect timeout, not per-chunk send timeout. */
        cfg.network_timeout_ms = CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS;
        cfg.reconnect_timeout_ms = CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS;
        cfg.disable_auto_reconnect = true;
        cfg.buffer_size = 4096;
        cfg.task_stack = WS_CLIENT_TASK_STACK_BYTES;

    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    if (err != ESP_OK) {
        (void)esp_websocket_client_destroy(client);
        return err;
    }

    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        (void)esp_websocket_client_destroy(client);
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    s_ws.client = client;
    s_ws.connected = false;
    s_ws.session_active = false;
    s_ws.start_sent = false;
    s_ws.active_capture_id = capture_id;
    s_ws.sample_rate_hz = sample_rate_hz;
    s_ws.result_ready = false;
    s_ws.result_capture_id = 0U;
    s_ws.result_intent = ORB_INTENT_UNKNOWN;
    s_ws.result_conf_permille = 0U;
    s_ws.mode = MIC_WS_MODE_KWS;
    s_ws.tts_active = false;
    s_ws.tts_done = false;
    s_ws.tts_failed = false;
    s_ws.tts_chunk_cb = NULL;
    s_ws.tts_chunk_cb_ctx = NULL;
    s_ws.tts_chunks_sent = 0U;
    s_ws.tts_chunks_dropped = 0U;
    portEXIT_CRITICAL(&s_lock);

    TickType_t deadline = xTaskGetTickCount() + ms_to_ticks_min1(CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS);
    while (!time_reached(xTaskGetTickCount(), deadline)) {
        if (esp_websocket_client_is_connected(client)) {
            break;
        }
        vTaskDelay(ms_to_ticks_min1(10U));
    }

    if (!esp_websocket_client_is_connected(client)) {
        mic_ws_client_abort();
        return ESP_ERR_TIMEOUT;
    }

    err = ws_send_start_frame(client, capture_id, sample_rate_hz);
    if (err != ESP_OK) {
        mic_ws_client_abort();
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    s_ws.session_active = true;
    s_ws.start_sent = true;
    portEXIT_CRITICAL(&s_lock);
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
    portENTER_CRITICAL(&s_lock);
    client = s_ws.client;
    session_active = s_ws.session_active;
    connected = s_ws.connected;
    start_sent = s_ws.start_sent;
    capture_id = s_ws.active_capture_id;
    sample_rate_hz = s_ws.sample_rate_hz;
    portEXIT_CRITICAL(&s_lock);

    if (client == NULL || !session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!connected || !esp_websocket_client_is_connected(client)) {
        if (!ws_wait_connected(client, CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS)) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (!start_sent) {
        esp_err_t start_err = ws_send_start_frame(client, capture_id, sample_rate_hz);
        if (start_err != ESP_OK) {
            return start_err;
        }
        portENTER_CRITICAL(&s_lock);
        s_ws.start_sent = true;
        portEXIT_CRITICAL(&s_lock);
    }

    return mic_ws_transport_send_all_bin(client,
                                         samples,
                                         sample_count,
                                         ws_effective_send_timeout_ms(),
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
    portENTER_CRITICAL(&s_lock);
    client = s_ws.client;
    capture_id = s_ws.active_capture_id;
    session_active = s_ws.session_active;
    portEXIT_CRITICAL(&s_lock);

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
                                            ws_effective_send_timeout_ms(),
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

    TickType_t deadline = xTaskGetTickCount() + ms_to_ticks_min1(timeout_ms);
    while (!time_reached(xTaskGetTickCount(), deadline)) {
        bool ready = false;
        uint32_t result_capture_id = 0U;
        orb_intent_id_t intent_id = ORB_INTENT_UNKNOWN;
        uint16_t conf = 0U;

        portENTER_CRITICAL(&s_lock);
        if (s_ws.result_ready) {
            ready = true;
            result_capture_id = s_ws.result_capture_id;
            intent_id = s_ws.result_intent;
            conf = s_ws.result_conf_permille;
        }
        portEXIT_CRITICAL(&s_lock);

        if (ready && (result_capture_id == capture_id || result_capture_id == 0U)) {
            *out_intent = intent_id;
            *out_confidence_permille = conf;
            portENTER_CRITICAL(&s_lock);
            s_ws.result_ready = false;
            s_ws.session_active = false;
            s_ws.start_sent = false;
            portEXIT_CRITICAL(&s_lock);
            return ESP_OK;
        }

        vTaskDelay(ms_to_ticks_min1(WS_RESULT_POLL_MS));
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t mic_ws_client_tts_play(const char *text,
                                 uint32_t sample_rate_hz,
                                 uint32_t timeout_ms,
                                 mic_ws_tts_chunk_cb_t chunk_cb,
                                 void *chunk_cb_ctx)
{
    if (!mic_ws_client_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (text == NULL || text[0] == '\0' || chunk_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0U) {
        timeout_ms = CONFIG_ORB_MIC_WS_TTS_TIMEOUT_MS;
    }
    if (!s_ws.initialized) {
        ESP_RETURN_ON_ERROR(mic_ws_client_init(), TAG, "mic ws init failed");
    }

    esp_websocket_client_handle_t client = NULL;
    bool reuse_connection = false;
    bool connected = false;
    portENTER_CRITICAL(&s_lock);
    client = s_ws.client;
    connected = s_ws.connected;
    portEXIT_CRITICAL(&s_lock);
    if (client != NULL && connected && esp_websocket_client_is_connected(client)) {
        reuse_connection = true;
    } else {
        mic_ws_client_abort();

        esp_websocket_client_config_t cfg = { 0 };
        cfg.uri = CONFIG_ORB_MIC_WS_URL;
        cfg.network_timeout_ms = CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS;
        cfg.reconnect_timeout_ms = CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS;
        cfg.disable_auto_reconnect = true;
        cfg.buffer_size = 4096;
        cfg.task_stack = WS_CLIENT_TASK_STACK_BYTES;

        client = esp_websocket_client_init(&cfg);
        if (client == NULL) {
            return ESP_FAIL;
        }

        esp_err_t init_err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
        if (init_err != ESP_OK) {
            (void)esp_websocket_client_destroy(client);
            return init_err;
        }

        init_err = esp_websocket_client_start(client);
        if (init_err != ESP_OK) {
            (void)esp_websocket_client_destroy(client);
            return init_err;
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_ws.client = client;
    s_ws.connected = reuse_connection ? true : false;
    s_ws.session_active = false;
    s_ws.start_sent = false;
    s_ws.active_capture_id = 0U;
    s_ws.sample_rate_hz = sample_rate_hz;
    s_ws.result_ready = false;
    s_ws.mode = MIC_WS_MODE_TTS;
    s_ws.tts_active = true;
    s_ws.tts_done = false;
    s_ws.tts_failed = false;
    s_ws.tts_chunk_cb = chunk_cb;
    s_ws.tts_chunk_cb_ctx = chunk_cb_ctx;
    s_ws.tts_chunks_sent = 0U;
    s_ws.tts_chunks_dropped = 0U;
    s_ws.tts_bytes_rx = 0U;
    s_ws.tts_frames_rx = 0U;
    s_ws.tts_started_tick = xTaskGetTickCount();
    s_ws.tts_last_diag_tick = 0U;
    s_ws.tts_pcm_tail_count = 0U;
    s_ws.rx_len = 0U;
    portEXIT_CRITICAL(&s_lock);

    if (!reuse_connection && !ws_wait_connected(client, CONFIG_ORB_MIC_WS_CONNECT_TIMEOUT_MS)) {
        mic_ws_client_abort();
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ws_send_tts_request(client, text, sample_rate_hz);
    if (err != ESP_OK) {
        mic_ws_client_abort();
        return err;
    }

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t deadline = start_tick + ms_to_ticks_min1(timeout_ms);
    TickType_t first_chunk_deadline = start_tick + ms_to_ticks_min1(CONFIG_ORB_MIC_WS_TTS_FIRST_CHUNK_TIMEOUT_MS);
    while (!time_reached(xTaskGetTickCount(), deadline)) {
        bool done = false;
        bool failed = false;
        bool connected = false;
        uint32_t sent = 0U;
        uint32_t dropped = 0U;
        uint32_t bytes = 0U;
        uint32_t frames = 0U;
        TickType_t started_tick = 0;
        portENTER_CRITICAL(&s_lock);
        done = s_ws.tts_done;
        failed = s_ws.tts_failed;
        connected = s_ws.connected;
        sent = s_ws.tts_chunks_sent;
        dropped = s_ws.tts_chunks_dropped;
        bytes = s_ws.tts_bytes_rx;
        frames = s_ws.tts_frames_rx;
        started_tick = s_ws.tts_started_tick;
        portEXIT_CRITICAL(&s_lock);

        TickType_t now_tick = xTaskGetTickCount();
        if (frames == 0U && !done && !failed && connected && time_reached(now_tick, first_chunk_deadline)) {
            uint32_t wait_ms = (uint32_t)((now_tick - start_tick) * portTICK_PERIOD_MS);
            ESP_LOGW(TAG,
                     "mic ws tts no first audio chunk within %" PRIu32 "ms (waited=%" PRIu32 "ms), abort",
                     (uint32_t)CONFIG_ORB_MIC_WS_TTS_FIRST_CHUNK_TIMEOUT_MS,
                     wait_ms);
            mic_ws_client_abort();
            return ESP_ERR_TIMEOUT;
        }

        if (done) {
            uint32_t wall_ms = (uint32_t)((xTaskGetTickCount() - started_tick) * portTICK_PERIOD_MS);
            uint32_t audio_ms =
                (sample_rate_hz > 0U) ? (bytes * 1000U / (sample_rate_hz * (uint32_t)sizeof(int16_t))) : 0U;
            if (frames == 0U || bytes == 0U) {
                ESP_LOGW(TAG,
                         "mic ws tts done without audio payload wall_ms=%" PRIu32
                         " frames=%" PRIu32 " bytes=%" PRIu32,
                         wall_ms,
                         frames,
                         bytes);
                mic_ws_client_abort();
                return ESP_ERR_INVALID_RESPONSE;
            }
            ESP_LOGD(TAG,
                     "mic ws tts complete wall_ms=%" PRIu32 " audio_ms=%" PRIu32
                     " frames=%" PRIu32 " chunks_ok=%" PRIu32 " chunks_drop=%" PRIu32,
                     wall_ms,
                     audio_ms,
                     frames,
                     sent,
                     dropped);
            mic_ws_client_abort();
            return ESP_OK;
        }
        if (failed || !connected) {
            ESP_LOGW(TAG,
                     "mic ws tts failed state: failed=%d connected=%d frames=%" PRIu32
                     " bytes=%" PRIu32 " chunks_ok=%" PRIu32 " chunks_drop=%" PRIu32,
                     failed ? 1 : 0,
                     connected ? 1 : 0,
                     frames,
                     bytes,
                     sent,
                     dropped);
            mic_ws_client_abort();
            return ESP_FAIL;
        }
        vTaskDelay(ms_to_ticks_min1(10U));
    }

    mic_ws_client_abort();
    return ESP_ERR_TIMEOUT;
}
