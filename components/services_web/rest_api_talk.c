#include "rest_api_modules.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "app_tasking.h"
#include "config_manager.h"
#include "mic_service.h"
#include "mic_types.h"
#include "rest_api_common.h"
#include "sdkconfig.h"
#include "session_controller.h"

static const char *TAG = LOG_TAG_REST;

#define TALK_TEXT_MAX_CHARS 320U
#define TALK_TEXT_ENCODED_MAX_CHARS 2048U
#define TALK_DEFAULT_STREAM_TIMEOUT_MS 90000U
#define TALK_MIN_STREAM_TIMEOUT_MS 1000U
#define TALK_MAX_STREAM_TIMEOUT_MS 180000U
#define TALK_BG_GAIN_SWITCH_FADE_MS 250U
#define TALK_FORM_BODY_MAX_CHARS 2304U
#define TALK_BODY_READ_DEADLINE_MS 2500U
#if CONFIG_HTTPD_WS_SUPPORT
#define ORB_TALK_WS_ENABLED 1
#else
#define ORB_TALK_WS_ENABLED 0
#endif

#if ORB_TALK_WS_ENABLED
#define TALK_LIVE_WS_PATH "/ws/talk"
#define TALK_LIVE_SAMPLE_RATE_HZ 44100U
#define TALK_LIVE_INACTIVITY_TIMEOUT_MS 5000U
#define TALK_LIVE_WATCHDOG_PERIOD_MS 250U
#define TALK_LIVE_MAX_RX_BYTES (AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES * sizeof(int16_t) * 6U)
#define TALK_LIVE_PREBUFFER_SAMPLES (TALK_LIVE_SAMPLE_RATE_HZ / 2U) /* 0.5s pre-roll before start */
#define TALK_LIVE_RING_CAPACITY_SAMPLES (TALK_LIVE_SAMPLE_RATE_HZ)  /* 1.0s total ring capacity */
#define TALK_LIVE_FEED_TASK_STACK 6144U
#define TALK_LIVE_FEED_TASK_PRIORITY 2U
#define TALK_LIVE_FEED_IDLE_MS 2U
#define TALK_LIVE_POSTFX_DECLICK_THRESHOLD 900
#define TALK_LIVE_POSTFX_DECLICK_RAMP_SAMPLES 24U
#define TALK_LIVE_POSTFX_DC_BETA_Q15 32604
#define TALK_LIVE_POSTFX_LIMITER_THRESHOLD 28000
#define TALK_LIVE_POSTFX_LIMITER_KNEE 2400
#endif

#if ORB_TALK_WS_ENABLED
typedef struct {
    bool has_last_sample;
    int16_t last_sample;
    int32_t dc_prev_x;
    int32_t dc_prev_y;
} talk_live_postfx_state_t;

typedef struct {
    SemaphoreHandle_t lock;
    esp_timer_handle_t watchdog;
    bool websocket_open;
    bool pcm_stream_active;
    int sockfd;
    int64_t last_chunk_ms;
    int16_t *ring_samples;
    uint32_t ring_capacity_samples;
    uint32_t ring_write_pos;
    uint32_t ring_read_pos;
    uint32_t ring_count;
    uint32_t ring_dropped_samples;
    uint32_t rx_chunks;
    uint32_t rx_samples;
    int64_t rx_diag_last_ms;
    TaskHandle_t feeder_task;
    bool feeder_stack_psram;
    talk_live_postfx_state_t postfx;
} talk_live_state_t;

static talk_live_state_t s_live = {
    .lock = NULL,
    .watchdog = NULL,
    .websocket_open = false,
    .pcm_stream_active = false,
    .sockfd = -1,
    .last_chunk_ms = 0,
    .ring_samples = NULL,
    .ring_capacity_samples = 0U,
    .ring_write_pos = 0U,
    .ring_read_pos = 0U,
    .ring_count = 0U,
    .ring_dropped_samples = 0U,
    .rx_chunks = 0U,
    .rx_samples = 0U,
    .rx_diag_last_ms = 0,
    .feeder_task = NULL,
    .feeder_stack_psram = false,
    .postfx = {
        .has_last_sample = false,
        .last_sample = 0,
        .dc_prev_x = 0,
        .dc_prev_y = 0,
    },
};
#endif

static SemaphoreHandle_t s_talk_say_lock;
static char s_talk_form_body[TALK_FORM_BODY_MAX_CHARS + 1U];
static char s_talk_text_encoded[TALK_TEXT_ENCODED_MAX_CHARS + 1U];

static uint32_t request_timeout_ms(void)
{
    return (uint32_t)CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
}

static uint16_t talk_bg_gain_for_tts(uint16_t configured_gain_permille)
{
    return (configured_gain_permille > 180U) ? 180U : configured_gain_permille;
}

static esp_err_t talk_start_bg_for_say(const orb_runtime_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_command_t cmd = { 0 };
    cmd.id = AUDIO_CMD_BG_SET_GAIN;
    /* Match hybrid "answer" transition profile for spoken response. */
    cmd.payload.bg_set_gain.fade_ms = TALK_BG_GAIN_SWITCH_FADE_MS;
    cmd.payload.bg_set_gain.gain_permille = talk_bg_gain_for_tts(cfg->prophecy_bg_gain_permille);
    return app_tasking_send_audio_command(&cmd, request_timeout_ms());
}

static esp_err_t talk_say_lock(void)
{
    if (s_talk_say_lock == NULL) {
        s_talk_say_lock = xSemaphoreCreateMutex();
        if (s_talk_say_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    if (ticks == 0) {
        ticks = 1;
    }
    if (xSemaphoreTake(s_talk_say_lock, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void talk_say_unlock(void)
{
    if (s_talk_say_lock != NULL) {
        xSemaphoreGive(s_talk_say_lock);
    }
}

static esp_err_t talk_read_form_body(httpd_req_t *req, char *out, size_t out_len)
{
    if (req == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (req->content_len <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t)req->content_len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    int remaining = req->content_len;
    int total = 0;
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)TALK_BODY_READ_DEADLINE_MS * 1000LL);
    while (remaining > 0) {
        int read_len = httpd_req_recv(req, out + total, remaining);
        if (read_len <= 0) {
            if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
                if (esp_timer_get_time() >= deadline_us) {
                    return ESP_ERR_TIMEOUT;
                }
                continue;
            }
            return ESP_FAIL;
        }
        total += read_len;
        remaining -= read_len;
    }
    out[total] = '\0';
    return ESP_OK;
}

static esp_err_t talk_get_param(httpd_req_t *req, const char *form_body, const char *key, char *out, size_t out_len)
{
    if (req == NULL || key == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t qerr = rest_api_query_value(req, key, out, out_len);
    if (qerr == ESP_OK) {
        return ESP_OK;
    }
    if (form_body != NULL && form_body[0] != '\0') {
        esp_err_t ferr = httpd_query_key_value(form_body, key, out, out_len);
        if (ferr == ESP_OK) {
            return ESP_OK;
        }
        if (ferr == ESP_ERR_HTTPD_RESULT_TRUNC) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static int talk_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return 10 + (int)(c - 'a');
    }
    return -1;
}

static bool talk_url_decode_inplace(char *text)
{
    if (text == NULL) {
        return false;
    }
    char *src = text;
    char *dst = text;
    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        if (*src == '%') {
            if (src[1] == '\0' || src[2] == '\0') {
                return false;
            }
            int hi = talk_hex_nibble(src[1]);
            int lo = talk_hex_nibble(src[2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    return true;
}

#if ORB_TALK_WS_ENABLED
static esp_err_t talk_live_lock(void)
{
    if (s_live.lock == NULL) {
        s_live.lock = xSemaphoreCreateMutex();
        if (s_live.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    if (ticks == 0) {
        ticks = 1;
    }
    if (xSemaphoreTake(s_live.lock, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void talk_live_unlock(void)
{
    if (s_live.lock != NULL) {
        xSemaphoreGive(s_live.lock);
    }
}

static esp_err_t talk_live_send_pcm_start(uint32_t timeout_ms)
{
    audio_command_t cmd = { .id = AUDIO_CMD_PCM_STREAM_START };
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

static esp_err_t talk_live_send_pcm_stop(uint32_t timeout_ms)
{
    audio_command_t cmd = { .id = AUDIO_CMD_PCM_STREAM_STOP };
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

static esp_err_t talk_live_send_pcm_chunk(const int16_t *samples, uint16_t sample_count, uint32_t timeout_ms)
{
    if (samples == NULL || sample_count == 0U || sample_count > AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_command_t cmd = { 0 };
    cmd.id = AUDIO_CMD_PCM_STREAM_CHUNK;
    cmd.payload.pcm_stream_chunk.sample_count = sample_count;
    (void)memcpy(cmd.payload.pcm_stream_chunk.samples,
                 samples,
                 (size_t)sample_count * sizeof(int16_t));
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

static inline int16_t talk_live_clamp_i16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

static void talk_live_postfx_reset_locked(void)
{
    s_live.postfx.has_last_sample = false;
    s_live.postfx.last_sample = 0;
    s_live.postfx.dc_prev_x = 0;
    s_live.postfx.dc_prev_y = 0;
}

static int16_t talk_live_postfx_soft_limit(int32_t x)
{
    int32_t ax = (x < 0) ? -x : x;
    if (ax <= TALK_LIVE_POSTFX_LIMITER_THRESHOLD) {
        return talk_live_clamp_i16(x);
    }
    int32_t over = ax - TALK_LIVE_POSTFX_LIMITER_THRESHOLD;
    int32_t comp = TALK_LIVE_POSTFX_LIMITER_THRESHOLD;
    comp += (int32_t)(((int64_t)over * (int64_t)TALK_LIVE_POSTFX_LIMITER_KNEE) /
                      (int64_t)(TALK_LIVE_POSTFX_LIMITER_KNEE + over));
    if (comp > 32767) {
        comp = 32767;
    }
    return (x < 0) ? (int16_t)(-comp) : (int16_t)comp;
}

static void talk_live_postfx_apply_inplace_locked(int16_t *samples, uint32_t sample_count)
{
    if (samples == NULL || sample_count == 0U) {
        return;
    }

    if (s_live.postfx.has_last_sample) {
        int32_t first = (int32_t)samples[0];
        int32_t prev = (int32_t)s_live.postfx.last_sample;
        int32_t jump = first - prev;
        if (jump < 0) {
            jump = -jump;
        }
        if (jump >= TALK_LIVE_POSTFX_DECLICK_THRESHOLD) {
            uint32_t ramp_n = sample_count;
            if (ramp_n > TALK_LIVE_POSTFX_DECLICK_RAMP_SAMPLES) {
                ramp_n = TALK_LIVE_POSTFX_DECLICK_RAMP_SAMPLES;
            }
            for (uint32_t i = 0U; i < ramp_n; ++i) {
                int32_t target = (int32_t)samples[i];
                int32_t mixed = prev + (int32_t)(((int64_t)(target - prev) * (int64_t)(i + 1U)) / (int64_t)ramp_n);
                samples[i] = talk_live_clamp_i16(mixed);
            }
        }
    }

    int32_t prev_x = s_live.postfx.dc_prev_x;
    int32_t prev_y = s_live.postfx.dc_prev_y;
    for (uint32_t i = 0U; i < sample_count; ++i) {
        int32_t x = (int32_t)samples[i];
        int32_t y = (x - prev_x) +
                    (int32_t)(((int64_t)TALK_LIVE_POSTFX_DC_BETA_Q15 * (int64_t)prev_y) >> 15);
        prev_x = x;
        prev_y = y;
        samples[i] = talk_live_postfx_soft_limit(y);
    }

    s_live.postfx.dc_prev_x = prev_x;
    s_live.postfx.dc_prev_y = prev_y;
    s_live.postfx.last_sample = samples[sample_count - 1U];
    s_live.postfx.has_last_sample = true;
}

static void talk_live_ring_reset_locked(void)
{
    s_live.ring_write_pos = 0U;
    s_live.ring_read_pos = 0U;
    s_live.ring_count = 0U;
}

static uint32_t talk_live_ring_push_locked(const int16_t *samples, uint32_t sample_count)
{
    if (samples == NULL || sample_count == 0U || s_live.ring_samples == NULL || s_live.ring_capacity_samples == 0U) {
        return 0U;
    }

    uint32_t accepted = 0U;
    while (accepted < sample_count) {
        if (s_live.ring_count >= s_live.ring_capacity_samples) {
            s_live.ring_dropped_samples += (sample_count - accepted);
            break;
        }
        s_live.ring_samples[s_live.ring_write_pos] = samples[accepted];
        s_live.ring_write_pos++;
        if (s_live.ring_write_pos >= s_live.ring_capacity_samples) {
            s_live.ring_write_pos = 0U;
        }
        s_live.ring_count++;
        accepted++;
    }
    return accepted;
}

static uint16_t talk_live_ring_pop_locked(int16_t *out_samples, uint16_t max_samples)
{
    if (out_samples == NULL || max_samples == 0U || s_live.ring_samples == NULL || s_live.ring_capacity_samples == 0U) {
        return 0U;
    }
    uint32_t to_copy = s_live.ring_count;
    if (to_copy > max_samples) {
        to_copy = max_samples;
    }
    uint16_t copied = 0U;
    while (copied < to_copy) {
        out_samples[copied++] = s_live.ring_samples[s_live.ring_read_pos];
        s_live.ring_read_pos++;
        if (s_live.ring_read_pos >= s_live.ring_capacity_samples) {
            s_live.ring_read_pos = 0U;
        }
        s_live.ring_count--;
    }
    return copied;
}

static void talk_live_feeder_task(void *arg)
{
    (void)arg;
    int16_t *chunk = (int16_t *)heap_caps_malloc((size_t)AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES * sizeof(int16_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        chunk = (int16_t *)heap_caps_malloc((size_t)AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES * sizeof(int16_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (chunk == NULL) {
        ESP_LOGE(TAG, "talk_live_feed chunk buffer alloc failed");
        if (talk_live_lock() == ESP_OK) {
            s_live.feeder_task = NULL;
            s_live.feeder_stack_psram = false;
            talk_live_unlock();
        }
        vTaskDelete(NULL);
        return;
    }
    TickType_t idle_ticks = pdMS_TO_TICKS(TALK_LIVE_FEED_IDLE_MS);
    if (idle_ticks == 0) {
        idle_ticks = 1;
    }

    for (;;) {
        bool need_start = false;
        bool need_stop = false;
        uint16_t chunk_samples = 0U;
        uint32_t buffered_samples = 0U;

        if (talk_live_lock() != ESP_OK) {
            vTaskDelay(idle_ticks);
            continue;
        }

        if (s_live.websocket_open && !s_live.pcm_stream_active && s_live.ring_count >= TALK_LIVE_PREBUFFER_SAMPLES) {
            s_live.pcm_stream_active = true;
            need_start = true;
            buffered_samples = s_live.ring_count;
        }

        if (s_live.pcm_stream_active) {
            chunk_samples = talk_live_ring_pop_locked(chunk, AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES);
            if (chunk_samples == 0U && !s_live.websocket_open) {
                s_live.pcm_stream_active = false;
                need_stop = true;
            }
        }

        talk_live_unlock();

        if (need_start) {
            esp_err_t start_err = talk_live_send_pcm_start(request_timeout_ms());
            if (start_err != ESP_OK) {
                ESP_LOGW(TAG, "live feeder pcm start failed: %s", esp_err_to_name(start_err));
                if (talk_live_lock() == ESP_OK) {
                    s_live.websocket_open = false;
                    s_live.pcm_stream_active = false;
                    s_live.sockfd = -1;
                    talk_live_ring_reset_locked();
                    talk_live_postfx_reset_locked();
                    talk_live_unlock();
                }
            } else {
                uint32_t buffered_ms = (uint32_t)(((uint64_t)buffered_samples * 1000ULL) / TALK_LIVE_SAMPLE_RATE_HZ);
                ESP_LOGI(TAG, "live feeder start playback buffered=%" PRIu32 " samples (%" PRIu32 " ms)",
                         buffered_samples,
                         buffered_ms);
            }
        }

        if (chunk_samples > 0U) {
            esp_err_t chunk_err = talk_live_send_pcm_chunk(chunk, chunk_samples, request_timeout_ms());
            if (chunk_err != ESP_OK) {
                ESP_LOGW(TAG, "live feeder chunk send failed: %s", esp_err_to_name(chunk_err));
            }
        } else if (!need_start && !need_stop) {
            vTaskDelay(idle_ticks);
        }

        if (need_stop) {
            esp_err_t stop_err = talk_live_send_pcm_stop(request_timeout_ms());
            if (stop_err != ESP_OK) {
                ESP_LOGW(TAG, "live feeder pcm stop failed: %s", esp_err_to_name(stop_err));
            } else {
                ESP_LOGI(TAG, "live feeder playback stopped");
            }
        }

        /* Yield CPU each loop but don't add fixed delay that throttles stream draining. */
        taskYIELD();
    }
}

static void talk_live_watchdog_cb(void *arg)
{
    (void)arg;
    if (s_live.lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_live.lock, 0) != pdTRUE) {
        return;
    }

    bool should_stop = false;
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    if (s_live.websocket_open && s_live.last_chunk_ms > 0 &&
        (now_ms - s_live.last_chunk_ms) > (int64_t)TALK_LIVE_INACTIVITY_TIMEOUT_MS) {
        should_stop = s_live.pcm_stream_active;
        s_live.pcm_stream_active = false;
        s_live.websocket_open = false;
        s_live.sockfd = -1;
        talk_live_ring_reset_locked();
        talk_live_postfx_reset_locked();
        s_live.ring_dropped_samples = 0U;
    }
    xSemaphoreGive(s_live.lock);

    if (should_stop) {
        esp_err_t err = talk_live_send_pcm_stop(0U);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "live watchdog stop failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "live watchdog stop: no chunks for %" PRIu32 " ms", (uint32_t)TALK_LIVE_INACTIVITY_TIMEOUT_MS);
        }
    }
}

static esp_err_t talk_live_init(void)
{
    if (talk_live_lock() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    talk_live_unlock();

    if (s_live.watchdog == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = talk_live_watchdog_cb,
            .arg = NULL,
            .name = "talk_live_wd",
            .dispatch_method = ESP_TIMER_TASK,
            .skip_unhandled_events = true,
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_live.watchdog), TAG, "live watchdog create failed");
        ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_live.watchdog, TALK_LIVE_WATCHDOG_PERIOD_MS * 1000ULL),
                            TAG,
                            "live watchdog start failed");
    }

    if (s_live.ring_samples == NULL) {
        s_live.ring_capacity_samples = TALK_LIVE_RING_CAPACITY_SAMPLES;
        s_live.ring_samples = (int16_t *)heap_caps_malloc((size_t)s_live.ring_capacity_samples * sizeof(int16_t),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_live.ring_samples == NULL) {
            s_live.ring_samples = (int16_t *)heap_caps_malloc((size_t)s_live.ring_capacity_samples * sizeof(int16_t),
                                                              MALLOC_CAP_8BIT);
        }
        if (s_live.ring_samples == NULL) {
            return ESP_ERR_NO_MEM;
        }
        talk_live_ring_reset_locked();
        talk_live_postfx_reset_locked();
        s_live.ring_dropped_samples = 0U;
    }

    if (s_live.feeder_task == NULL) {
        BaseType_t ok = pdFAIL;
        s_live.feeder_stack_psram = false;
#if CONFIG_SPIRAM_USE_MALLOC
#if CONFIG_FREERTOS_UNICORE
        ok = xTaskCreateWithCaps(talk_live_feeder_task,
                                 "talk_live_feed",
                                 TALK_LIVE_FEED_TASK_STACK,
                                 NULL,
                                 TALK_LIVE_FEED_TASK_PRIORITY,
                                 &s_live.feeder_task,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        ok = xTaskCreatePinnedToCoreWithCaps(talk_live_feeder_task,
                                             "talk_live_feed",
                                             TALK_LIVE_FEED_TASK_STACK,
                                             NULL,
                                             TALK_LIVE_FEED_TASK_PRIORITY,
                                             &s_live.feeder_task,
                                             tskNO_AFFINITY,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (ok == pdPASS && s_live.feeder_task != NULL) {
            s_live.feeder_stack_psram = true;
        } else {
            s_live.feeder_task = NULL;
        }
#endif
        if (ok != pdPASS || s_live.feeder_task == NULL) {
            s_live.feeder_stack_psram = false;
            ok = xTaskCreatePinnedToCore(talk_live_feeder_task,
                                         "talk_live_feed",
                                         TALK_LIVE_FEED_TASK_STACK,
                                         NULL,
                                         TALK_LIVE_FEED_TASK_PRIORITY,
                                         &s_live.feeder_task,
                                         tskNO_AFFINITY);
        }
        if (ok != pdPASS || s_live.feeder_task == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}
#else
static esp_err_t talk_live_init(void)
{
    return ESP_OK;
}

static esp_err_t talk_live_snapshot(bool *open_out, bool *stream_out)
{
    if (open_out == NULL || stream_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *open_out = false;
    *stream_out = false;
    return ESP_OK;
}
#endif

#if ORB_TALK_WS_ENABLED
static esp_err_t talk_live_snapshot(bool *open_out, bool *stream_out)
{
    if (open_out == NULL || stream_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(talk_live_lock(), TAG, "live lock failed");
    *open_out = s_live.websocket_open;
    *stream_out = s_live.pcm_stream_active;
    talk_live_unlock();
    return ESP_OK;
}

static esp_err_t talk_live_stop_if_owner(int sockfd, bool close_socket_state)
{
    bool should_stop = false;
    ESP_RETURN_ON_ERROR(talk_live_lock(), TAG, "live lock failed");
    if (s_live.sockfd == sockfd || s_live.sockfd < 0) {
        should_stop = s_live.pcm_stream_active;
        s_live.pcm_stream_active = false;
        talk_live_ring_reset_locked();
        talk_live_postfx_reset_locked();
        if (close_socket_state) {
            s_live.websocket_open = false;
            s_live.sockfd = -1;
        }
    }
    talk_live_unlock();

    if (should_stop) {
        esp_err_t err = talk_live_send_pcm_stop(request_timeout_ms());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "live pcm stop failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t talk_live_mark_open(int sockfd)
{
    ESP_RETURN_ON_ERROR(talk_live_lock(), TAG, "live lock failed");
    if (s_live.websocket_open || s_live.sockfd >= 0) {
        talk_live_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_live.websocket_open = true;
    s_live.pcm_stream_active = false;
    s_live.sockfd = sockfd;
    s_live.last_chunk_ms = esp_timer_get_time() / 1000LL;
    s_live.ring_dropped_samples = 0U;
    s_live.rx_chunks = 0U;
    s_live.rx_samples = 0U;
    s_live.rx_diag_last_ms = 0;
    talk_live_ring_reset_locked();
    talk_live_postfx_reset_locked();
    talk_live_unlock();
    return ESP_OK;
}

static esp_err_t talk_live_accept_chunk_owner(int sockfd)
{
    ESP_RETURN_ON_ERROR(talk_live_lock(), TAG, "live lock failed");
    if (!s_live.websocket_open || s_live.sockfd != sockfd) {
        talk_live_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_live.last_chunk_ms = esp_timer_get_time() / 1000LL;
    talk_live_unlock();
    return ESP_OK;
}

static esp_err_t talk_live_send_pcm_chunk_bytes(uint8_t *data, size_t len)
{
    if (data == NULL || len < sizeof(int16_t)) {
        return ESP_OK;
    }

    size_t even_len = len & ~(size_t)1U;
    if (even_len == 0) {
        return ESP_OK;
    }
    int16_t *in_samples = (int16_t *)data;
    uint32_t sample_total = (uint32_t)(even_len / sizeof(int16_t));
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    bool emit_diag = false;
    uint32_t diag_chunks = 0U;
    uint32_t diag_samples = 0U;
    int16_t min_v = 32767;
    int16_t max_v = -32768;
    uint64_t sum_abs = 0U;
    uint32_t abs_avg = 0U;

    ESP_RETURN_ON_ERROR(talk_live_lock(), TAG, "live lock failed");
    talk_live_postfx_apply_inplace_locked(in_samples, sample_total);
    uint32_t accepted = talk_live_ring_push_locked(in_samples, sample_total);
    uint32_t dropped = sample_total - accepted;
    uint32_t dropped_total = s_live.ring_dropped_samples;
    s_live.rx_chunks++;
    s_live.rx_samples += sample_total;
    if ((now_ms - s_live.rx_diag_last_ms) >= 1000LL) {
        s_live.rx_diag_last_ms = now_ms;
        emit_diag = true;
        diag_chunks = s_live.rx_chunks;
        diag_samples = s_live.rx_samples;
    }
    for (uint32_t i = 0U; i < sample_total; ++i) {
        int16_t v = in_samples[i];
        if (v < min_v) {
            min_v = v;
        }
        if (v > max_v) {
            max_v = v;
        }
        int32_t iv = (int32_t)v;
        sum_abs += (uint64_t)((iv < 0) ? -iv : iv);
    }
    abs_avg = (sample_total > 0U) ? (uint32_t)(sum_abs / (uint64_t)sample_total) : 0U;
    talk_live_unlock();

    if (dropped > 0U) {
        ESP_LOGW(TAG,
                 "live ring overflow accepted=%" PRIu32 " dropped=%" PRIu32 " dropped_total=%" PRIu32,
                 accepted,
                 dropped,
                 dropped_total);
    }
    if (emit_diag) {
        uint32_t audio_ms = (uint32_t)(((uint64_t)diag_samples * 1000ULL) / TALK_LIVE_SAMPLE_RATE_HZ);
        ESP_LOGI(TAG,
                 "live rx diag chunks=%" PRIu32 " samples=%" PRIu32 " audio_ms=%" PRIu32
                 " frame[min=%d max=%d abs_avg=%" PRIu32 "]",
                 diag_chunks,
                 diag_samples,
                 audio_ms,
                 (int)min_v,
                 (int)max_v,
                 abs_avg);
    }
    return ESP_OK;
}
#endif

static esp_err_t talk_check_busy(bool *busy_out)
{
    if (busy_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *busy_out = false;

    session_info_t session = { 0 };
    esp_err_t sess_err = session_controller_get_info(&session);
    if (sess_err != ESP_OK) {
        return sess_err;
    }
    if (session.active) {
        *busy_out = true;
        return ESP_OK;
    }

    mic_capture_status_t mic = { 0 };
    esp_err_t mic_err = mic_service_get_status(&mic);
    if (mic_err == ESP_OK && mic.active) {
        *busy_out = true;
        return ESP_OK;
    }

    bool live_open = false;
    bool live_stream = false;
    esp_err_t live_err = talk_live_snapshot(&live_open, &live_stream);
    if (live_err == ESP_OK && (live_open || live_stream)) {
        *busy_out = true;
    }

    return ESP_OK;
}

static esp_err_t talk_say_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    if (talk_say_lock() != ESP_OK) {
        return rest_api_send_error_json(req, "409 Conflict", "talk_busy");
    }

    s_talk_form_body[0] = '\0';
    s_talk_text_encoded[0] = '\0';

    esp_err_t body_err = talk_read_form_body(req, s_talk_form_body, sizeof(s_talk_form_body));
    if (body_err == ESP_ERR_INVALID_SIZE) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "request_too_large");
        goto cleanup;
    }
    if (body_err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "talk say body read timeout content_len=%d", req ? req->content_len : -1);
        ret = rest_api_send_error_json(req, "408 Request Timeout", "request_body_timeout");
        goto cleanup;
    }
    if (body_err != ESP_OK && body_err != ESP_ERR_NOT_FOUND) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_request_body");
        goto cleanup;
    }

    esp_err_t text_err = talk_get_param(req, s_talk_form_body, "text", s_talk_text_encoded, sizeof(s_talk_text_encoded));
    if (text_err == ESP_ERR_INVALID_SIZE) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "text_too_long");
        goto cleanup;
    }
    if (text_err != ESP_OK || s_talk_text_encoded[0] == '\0') {
        ret = rest_api_send_error_json(req, "400 Bad Request", "missing_text");
        goto cleanup;
    }
    if (strchr(s_talk_text_encoded, '%') != NULL || strchr(s_talk_text_encoded, '+') != NULL) {
        if (!talk_url_decode_inplace(s_talk_text_encoded)) {
            ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_text_encoding");
            goto cleanup;
        }
    }
    if (s_talk_text_encoded[0] == '\0') {
        ret = rest_api_send_error_json(req, "400 Bad Request", "missing_text");
        goto cleanup;
    }
    if (strlen(s_talk_text_encoded) > TALK_TEXT_MAX_CHARS) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "text_too_long");
        goto cleanup;
    }
    if (strlen(s_talk_text_encoded) >= sizeof(((mic_command_t *)0)->payload.tts_play.text)) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "text_too_long");
        goto cleanup;
    }

    char text[TALK_TEXT_MAX_CHARS + 1U];
    (void)strncpy(text, s_talk_text_encoded, sizeof(text) - 1U);
    text[sizeof(text) - 1U] = '\0';

    uint32_t stream_timeout_ms = TALK_DEFAULT_STREAM_TIMEOUT_MS;
    bool with_bg = true;
    uint32_t bg_fade_out_ms = 0U;
    char timeout_text[16];
    char with_bg_text[16];
    if (talk_get_param(req, s_talk_form_body, "timeout_ms", timeout_text, sizeof(timeout_text)) == ESP_OK) {
        if (!rest_api_parse_u32(timeout_text, &stream_timeout_ms) ||
            stream_timeout_ms < TALK_MIN_STREAM_TIMEOUT_MS ||
            stream_timeout_ms > TALK_MAX_STREAM_TIMEOUT_MS) {
            ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_timeout_ms");
            goto cleanup;
        }
    }
    if (talk_get_param(req, s_talk_form_body, "with_bg", with_bg_text, sizeof(with_bg_text)) == ESP_OK) {
        if (!rest_api_parse_bool_text(with_bg_text, &with_bg)) {
            ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_with_bg");
            goto cleanup;
        }
    }

    if (!mic_service_is_enabled()) {
        ret = rest_api_send_error_json(req, "503 Service Unavailable", "mic_service_disabled");
        goto cleanup;
    }

    bool busy = false;
    esp_err_t busy_err = talk_check_busy(&busy);
    if (busy_err != ESP_OK) {
        ESP_LOGW(TAG, "talk busy check failed: %s", esp_err_to_name(busy_err));
        ret = rest_api_send_error_json(req, "500 Internal Server Error", "talk_busy_check_failed");
        goto cleanup;
    }
    if (busy) {
        ret = rest_api_send_error_json(req, "409 Conflict", "talk_busy");
        goto cleanup;
    }

    if (with_bg) {
        orb_runtime_config_t cfg = { 0 };
        if (config_manager_get_snapshot(&cfg) != ESP_OK) {
            ret = rest_api_send_error_json(req, "500 Internal Server Error", "config_read_failed");
            goto cleanup;
        }
        esp_err_t bg_err = talk_start_bg_for_say(&cfg);
        if (bg_err != ESP_OK) {
            ESP_LOGW(TAG, "talk say bg start failed: %s", esp_err_to_name(bg_err));
            ret = rest_api_send_error_json(req, "500 Internal Server Error", "talk_bg_start_failed");
            goto cleanup;
        }
        bg_fade_out_ms = cfg.prophecy_bg_fade_out_ms;
    }

    esp_err_t err = mic_service_play_tts_text(text, stream_timeout_ms, bg_fade_out_ms, request_timeout_ms());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "talk say failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_INVALID_SIZE) {
            ret = rest_api_send_error_json(req, "400 Bad Request", "text_too_long");
            goto cleanup;
        }
        ret = rest_api_send_error_json(req, "500 Internal Server Error", "talk_tts_failed");
        goto cleanup;
    }
    ESP_LOGI(TAG,
             "talk say accepted chars=%u timeout_ms=%lu with_bg=%u bg_fade_out_ms=%" PRIu32,
             (unsigned)strlen(text),
             (unsigned long)stream_timeout_ms,
             with_bg ? 1U : 0U,
             bg_fade_out_ms);

    char json[136];
    (void)snprintf(json,
                   sizeof(json),
                   "{\"ok\":true,\"chars\":%u,\"timeout_ms\":%lu,\"with_bg\":%s,\"bg_fade_out_ms\":%" PRIu32 "}",
                   (unsigned)strlen(text),
                   (unsigned long)stream_timeout_ms,
                   with_bg ? "true" : "false",
                   bg_fade_out_ms);
    ret = rest_api_send_json(req, "200 OK", json);

cleanup:
    talk_say_unlock();
    return ret;
}

#if ORB_TALK_WS_ENABLED
static esp_err_t talk_live_ws_handler(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        bool busy = false;
        esp_err_t busy_err = talk_check_busy(&busy);
        if (busy_err != ESP_OK) {
            return ESP_FAIL;
        }
        if (busy) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "talk_busy");
        }
        esp_err_t mark_err = talk_live_mark_open(sockfd);
        if (mark_err != ESP_OK) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "talk_busy");
        }
        ESP_LOGI(TAG, "live ws open fd=%d postfx=fast", sockfd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { 0 };
    uint8_t *payload_buf = NULL;
    esp_err_t recv_err = httpd_ws_recv_frame(req, &frame, 0);
    if (recv_err != ESP_OK) {
        ESP_LOGW(TAG, "live ws recv len failed fd=%d: %s", sockfd, esp_err_to_name(recv_err));
        (void)talk_live_stop_if_owner(sockfd, true);
        return recv_err;
    }

    if (frame.len > 0) {
        if (frame.len > TALK_LIVE_MAX_RX_BYTES) {
            ESP_LOGW(TAG, "live ws frame too large fd=%d len=%u", sockfd, (unsigned)frame.len);
            (void)talk_live_stop_if_owner(sockfd, true);
            return ESP_ERR_INVALID_SIZE;
        }
        payload_buf = (uint8_t *)heap_caps_malloc(frame.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (payload_buf == NULL) {
            payload_buf = (uint8_t *)heap_caps_malloc(frame.len, MALLOC_CAP_8BIT);
        }
        if (payload_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        frame.payload = payload_buf;
        recv_err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (recv_err != ESP_OK) {
            ESP_LOGW(TAG, "live ws recv payload failed fd=%d: %s", sockfd, esp_err_to_name(recv_err));
            (void)talk_live_stop_if_owner(sockfd, true);
            free(payload_buf);
            return recv_err;
        }
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "live ws close fd=%d", sockfd);
        (void)talk_live_stop_if_owner(sockfd, true);
        free(payload_buf);
        return ESP_OK;
    }

    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        if (frame.len >= 4U && memcmp(frame.payload, "stop", 4U) == 0) {
            ESP_LOGI(TAG, "live ws stop requested fd=%d", sockfd);
            (void)talk_live_stop_if_owner(sockfd, true);
        }
        free(payload_buf);
        return ESP_OK;
    }

    if (frame.type != HTTPD_WS_TYPE_BINARY) {
        free(payload_buf);
        return ESP_OK;
    }

    esp_err_t owner_err = talk_live_accept_chunk_owner(sockfd);
    if (owner_err != ESP_OK) {
        ESP_LOGW(TAG, "live ws chunk rejected fd=%d: %s", sockfd, esp_err_to_name(owner_err));
        free(payload_buf);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t chunk_err = talk_live_send_pcm_chunk_bytes(frame.payload, frame.len);
    if (chunk_err != ESP_OK) {
        ESP_LOGW(TAG, "live ws pcm chunk failed fd=%d: %s", sockfd, esp_err_to_name(chunk_err));
        (void)talk_live_stop_if_owner(sockfd, true);
        free(payload_buf);
        return chunk_err;
    }

    free(payload_buf);
    return ESP_OK;
}
#endif

esp_err_t rest_api_register_talk_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(talk_live_init(), TAG, "talk live init failed");

    const httpd_uri_t talk_say = {
        .uri = "/api/talk/say",
        .method = HTTP_POST,
        .handler = talk_say_handler,
        .user_ctx = NULL,
    };
#if ORB_TALK_WS_ENABLED
    const httpd_uri_t talk_live_ws = {
        .uri = TALK_LIVE_WS_PATH,
        .method = HTTP_GET,
        .handler = talk_live_ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };
#endif

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &talk_say), TAG, "register talk say failed");
#if ORB_TALK_WS_ENABLED
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &talk_live_ws), TAG, "register talk ws failed");
#endif
    return ESP_OK;
}
