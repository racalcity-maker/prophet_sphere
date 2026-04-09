#include "app_tasking.h"

#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_APP_TASKING;

static QueueHandle_t s_app_event_queue;
static QueueHandle_t s_led_cmd_queue;
static QueueHandle_t s_audio_cmd_queue;
static QueueHandle_t s_ai_cmd_queue;
static QueueHandle_t s_mic_cmd_queue;
static StaticQueue_t s_audio_cmd_queue_static_ctrl;
static uint8_t *s_audio_cmd_queue_storage;
static portMUX_TYPE s_timer_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_timer_pending_mask;

#ifndef CONFIG_ORB_MIC_COMMAND_QUEUE_LENGTH
#define CONFIG_ORB_MIC_COMMAND_QUEUE_LENGTH 8
#endif
#ifndef CONFIG_ORB_AUDIO_PCM_CHUNK_POOL_LENGTH
#define CONFIG_ORB_AUDIO_PCM_CHUNK_POOL_LENGTH 8
#endif

static QueueHandle_t s_audio_pcm_free_queue;
static audio_pcm_chunk_t **s_audio_pcm_chunks;

static void cleanup_audio_pcm_chunk_pool(void)
{
    if (s_audio_pcm_free_queue != NULL) {
        vQueueDelete(s_audio_pcm_free_queue);
        s_audio_pcm_free_queue = NULL;
    }
    if (s_audio_pcm_chunks != NULL) {
        const size_t chunk_pool_len = (size_t)CONFIG_ORB_AUDIO_PCM_CHUNK_POOL_LENGTH;
        for (size_t i = 0; i < chunk_pool_len; ++i) {
            if (s_audio_pcm_chunks[i] != NULL) {
                heap_caps_free(s_audio_pcm_chunks[i]);
            }
        }
        heap_caps_free(s_audio_pcm_chunks);
        s_audio_pcm_chunks = NULL;
    }
}

static uint32_t timer_kind_to_mask(app_timer_kind_t kind)
{
    if (kind <= APP_TIMER_KIND_NONE || kind >= 32) {
        return 0U;
    }
    return (1UL << (uint32_t)kind);
}

static void mark_pending_timer_event(app_timer_kind_t kind)
{
    const uint32_t mask = timer_kind_to_mask(kind);
    if (mask == 0U) {
        return;
    }
    portENTER_CRITICAL(&s_timer_pending_lock);
    s_timer_pending_mask |= mask;
    portEXIT_CRITICAL(&s_timer_pending_lock);
}

static void cleanup_queues(void)
{
    if (s_app_event_queue != NULL) {
        vQueueDelete(s_app_event_queue);
        s_app_event_queue = NULL;
    }
    if (s_led_cmd_queue != NULL) {
        vQueueDelete(s_led_cmd_queue);
        s_led_cmd_queue = NULL;
    }
    if (s_audio_cmd_queue != NULL) {
        vQueueDelete(s_audio_cmd_queue);
        s_audio_cmd_queue = NULL;
    }
    if (s_audio_cmd_queue_storage != NULL) {
        heap_caps_free(s_audio_cmd_queue_storage);
        s_audio_cmd_queue_storage = NULL;
    }
    if (s_audio_pcm_free_queue != NULL || s_audio_pcm_chunks != NULL) {
        cleanup_audio_pcm_chunk_pool();
    }
    if (s_ai_cmd_queue != NULL) {
        vQueueDelete(s_ai_cmd_queue);
        s_ai_cmd_queue = NULL;
    }
    if (s_mic_cmd_queue != NULL) {
        vQueueDelete(s_mic_cmd_queue);
        s_mic_cmd_queue = NULL;
    }
    portENTER_CRITICAL(&s_timer_pending_lock);
    s_timer_pending_mask = 0U;
    portEXIT_CRITICAL(&s_timer_pending_lock);
}

static audio_pcm_chunk_t *alloc_audio_pcm_chunk(void)
{
    audio_pcm_chunk_t *chunk = (audio_pcm_chunk_t *)heap_caps_malloc(sizeof(audio_pcm_chunk_t),
                                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        chunk = (audio_pcm_chunk_t *)heap_caps_malloc(sizeof(audio_pcm_chunk_t), MALLOC_CAP_8BIT);
    }
    if (chunk != NULL) {
        (void)memset(chunk, 0, sizeof(*chunk));
    }
    return chunk;
}

static esp_err_t create_audio_pcm_chunk_pool(void)
{
    if (s_audio_pcm_free_queue != NULL && s_audio_pcm_chunks != NULL) {
        return ESP_OK;
    }

    const size_t chunk_pool_len = (size_t)CONFIG_ORB_AUDIO_PCM_CHUNK_POOL_LENGTH;
    s_audio_pcm_free_queue = xQueueCreate((UBaseType_t)chunk_pool_len, sizeof(audio_pcm_chunk_t *));
    if (s_audio_pcm_free_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_audio_pcm_chunks =
        (audio_pcm_chunk_t **)heap_caps_calloc(chunk_pool_len, sizeof(audio_pcm_chunk_t *), MALLOC_CAP_8BIT);
    if (s_audio_pcm_chunks == NULL) {
        vQueueDelete(s_audio_pcm_free_queue);
        s_audio_pcm_free_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < chunk_pool_len; ++i) {
        audio_pcm_chunk_t *chunk = alloc_audio_pcm_chunk();
        if (chunk == NULL) {
            cleanup_audio_pcm_chunk_pool();
            return ESP_ERR_NO_MEM;
        }
        s_audio_pcm_chunks[i] = chunk;
        if (xQueueSend(s_audio_pcm_free_queue, &chunk, 0) != pdTRUE) {
            cleanup_audio_pcm_chunk_pool();
            return ESP_ERR_INVALID_STATE;
        }
    }

    const size_t bytes_total = chunk_pool_len * sizeof(audio_pcm_chunk_t);
    ESP_LOGI(TAG,
             "audio pcm pool chunks=%u bytes=%u",
             (unsigned)chunk_pool_len,
             (unsigned)bytes_total);
    return ESP_OK;
}

static QueueHandle_t create_audio_queue(void)
{
    const size_t storage_size =
        (size_t)CONFIG_ORB_AUDIO_COMMAND_QUEUE_LENGTH * sizeof(audio_command_t);

    uint8_t *storage = (uint8_t *)heap_caps_malloc(storage_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == NULL) {
        storage = (uint8_t *)heap_caps_malloc(storage_size, MALLOC_CAP_8BIT);
    }

    if (storage != NULL) {
        QueueHandle_t q = xQueueCreateStatic(CONFIG_ORB_AUDIO_COMMAND_QUEUE_LENGTH,
                                             sizeof(audio_command_t),
                                             storage,
                                             &s_audio_cmd_queue_static_ctrl);
        if (q != NULL) {
            s_audio_cmd_queue_storage = storage;
            ESP_LOGI(TAG,
                     "audio queue storage=%u bytes placed in %s memory",
                     (unsigned)storage_size,
                     esp_ptr_external_ram(storage) ? "PSRAM" : "internal");
            return q;
        }
        heap_caps_free(storage);
    }

    s_audio_cmd_queue_storage = NULL;
    return xQueueCreate(CONFIG_ORB_AUDIO_COMMAND_QUEUE_LENGTH, sizeof(audio_command_t));
}

static esp_err_t send_to_queue(QueueHandle_t queue, const void *item, uint32_t timeout_ms)
{
    if (queue == NULL || item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    BaseType_t ok = xQueueSend(queue, item, timeout_ticks);
    if (ok != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t app_tasking_init(void)
{
    bool all_ready = (s_app_event_queue != NULL && s_led_cmd_queue != NULL && s_audio_cmd_queue != NULL &&
                      s_ai_cmd_queue != NULL && s_mic_cmd_queue != NULL && s_audio_pcm_free_queue != NULL &&
                      s_audio_pcm_chunks != NULL);
    if (all_ready) {
        return ESP_OK;
    }

    if (s_app_event_queue != NULL || s_led_cmd_queue != NULL || s_audio_cmd_queue != NULL || s_ai_cmd_queue != NULL ||
        s_mic_cmd_queue != NULL || s_audio_pcm_free_queue != NULL || s_audio_pcm_chunks != NULL) {
        ESP_LOGW(TAG, "partial queue state detected, cleaning up and retrying init");
        cleanup_queues();
    }

    QueueHandle_t app_q = xQueueCreate(CONFIG_ORB_APP_EVENT_QUEUE_LENGTH, sizeof(app_event_t));
    QueueHandle_t led_q = xQueueCreate(CONFIG_ORB_LED_COMMAND_QUEUE_LENGTH, sizeof(led_command_t));
    QueueHandle_t audio_q = create_audio_queue();
    QueueHandle_t ai_q = xQueueCreate(CONFIG_ORB_AI_COMMAND_QUEUE_LENGTH, sizeof(ai_command_t));
    QueueHandle_t mic_q = xQueueCreate(CONFIG_ORB_MIC_COMMAND_QUEUE_LENGTH, sizeof(mic_command_t));
    esp_err_t pool_err = create_audio_pcm_chunk_pool();

    if (app_q == NULL || led_q == NULL || audio_q == NULL || ai_q == NULL || mic_q == NULL || pool_err != ESP_OK) {
        if (app_q != NULL) {
            vQueueDelete(app_q);
        }
        if (led_q != NULL) {
            vQueueDelete(led_q);
        }
        if (audio_q != NULL) {
            vQueueDelete(audio_q);
        }
        if (s_audio_cmd_queue_storage != NULL) {
            heap_caps_free(s_audio_cmd_queue_storage);
            s_audio_cmd_queue_storage = NULL;
        }
        if (ai_q != NULL) {
            vQueueDelete(ai_q);
        }
        if (mic_q != NULL) {
            vQueueDelete(mic_q);
        }
        cleanup_queues();
        ESP_LOGE(TAG, "queue allocation failed");
        return ESP_ERR_NO_MEM;
    }

    s_app_event_queue = app_q;
    s_led_cmd_queue = led_q;
    s_audio_cmd_queue = audio_q;
    s_ai_cmd_queue = ai_q;
    s_mic_cmd_queue = mic_q;

    ESP_LOGI(TAG,
             "queues ready app=%u led=%u audio=%u ai=%u mic=%u",
             (unsigned)CONFIG_ORB_APP_EVENT_QUEUE_LENGTH,
             (unsigned)CONFIG_ORB_LED_COMMAND_QUEUE_LENGTH,
             (unsigned)CONFIG_ORB_AUDIO_COMMAND_QUEUE_LENGTH,
             (unsigned)CONFIG_ORB_AI_COMMAND_QUEUE_LENGTH,
             (unsigned)CONFIG_ORB_MIC_COMMAND_QUEUE_LENGTH);
    return ESP_OK;
}

esp_err_t app_tasking_post_event(const app_event_t *event, uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
    }
    return send_to_queue(s_app_event_queue, event, timeout_ms);
}

esp_err_t app_tasking_post_timer_event_reliable(app_timer_kind_t timer_kind, uint32_t timeout_ms)
{
    if (timer_kind <= APP_TIMER_KIND_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    app_event_t event = { 0 };
    event.id = APP_EVENT_TIMER_EXPIRED;
    event.source = APP_EVENT_SOURCE_TIMER;
    event.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event.payload.scalar.code = (int32_t)timer_kind;

    esp_err_t err = app_tasking_post_event(&event, timeout_ms);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err != ESP_ERR_TIMEOUT) {
        return err;
    }

    mark_pending_timer_event(timer_kind);
    ESP_LOGW(TAG,
             "app_event_queue busy, deferred timer event kind=%d for guaranteed later delivery",
             (int)timer_kind);
    return ESP_OK;
}

bool app_tasking_take_pending_timer_event(app_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    uint32_t mask = 0U;
    portENTER_CRITICAL(&s_timer_pending_lock);
    mask = s_timer_pending_mask;
    if (mask != 0U) {
        uint32_t bit = (mask & (~mask + 1U));
        s_timer_pending_mask &= ~bit;
        mask = bit;
    }
    portEXIT_CRITICAL(&s_timer_pending_lock);

    if (mask == 0U) {
        return false;
    }

    uint32_t kind = (uint32_t)__builtin_ctz(mask);

    *event = (app_event_t){ 0 };
    event->id = APP_EVENT_TIMER_EXPIRED;
    event->source = APP_EVENT_SOURCE_TIMER;
    event->timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    event->payload.scalar.code = (int32_t)kind;
    return true;
}

esp_err_t app_tasking_send_led_command(const led_command_t *command, uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
    }
    return send_to_queue(s_led_cmd_queue, command, timeout_ms);
}

esp_err_t app_tasking_send_audio_command(const audio_command_t *command, uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
    }
    if (command != NULL &&
        command->id == AUDIO_CMD_PCM_STREAM_CHUNK &&
        command->payload.pcm_stream_chunk.chunk == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_to_queue(s_audio_cmd_queue, command, timeout_ms);
}

esp_err_t app_tasking_send_audio_pcm_chunk_copy(const int16_t *samples, uint16_t sample_count, uint32_t timeout_ms)
{
    if (samples == NULL || sample_count == 0U || sample_count > AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio_pcm_free_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0U) {
        timeout_ms = CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
    }
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    audio_pcm_chunk_t *chunk = NULL;
    if (xQueueReceive(s_audio_pcm_free_queue, &chunk, timeout_ticks) != pdTRUE || chunk == NULL) {
        return ESP_ERR_TIMEOUT;
    }

    chunk->sample_count = sample_count;
    (void)memcpy(chunk->samples, samples, (size_t)sample_count * sizeof(int16_t));

    audio_command_t cmd = { 0 };
    cmd.id = AUDIO_CMD_PCM_STREAM_CHUNK;
    cmd.payload.pcm_stream_chunk.chunk = chunk;
    esp_err_t err = app_tasking_send_audio_command(&cmd, timeout_ms);
    if (err != ESP_OK) {
        app_tasking_release_audio_pcm_chunk(chunk);
    }
    return err;
}

void app_tasking_release_audio_pcm_chunk(audio_pcm_chunk_t *chunk)
{
    if (chunk == NULL || s_audio_pcm_free_queue == NULL) {
        return;
    }
    if (xQueueSend(s_audio_pcm_free_queue, &chunk, 0) != pdTRUE) {
        ESP_LOGW(TAG, "audio pcm chunk pool overflow on release");
    }
}

esp_err_t app_tasking_send_ai_command(const ai_command_t *command, uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
    }
    return send_to_queue(s_ai_cmd_queue, command, timeout_ms);
}

esp_err_t app_tasking_send_mic_command(const mic_command_t *command, uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
    }
    return send_to_queue(s_mic_cmd_queue, command, timeout_ms);
}

QueueHandle_t app_tasking_get_app_event_queue(void)
{
    return s_app_event_queue;
}

QueueHandle_t app_tasking_get_led_cmd_queue(void)
{
    return s_led_cmd_queue;
}

QueueHandle_t app_tasking_get_audio_cmd_queue(void)
{
    return s_audio_cmd_queue;
}

QueueHandle_t app_tasking_get_ai_cmd_queue(void)
{
    return s_ai_cmd_queue;
}

QueueHandle_t app_tasking_get_mic_cmd_queue(void)
{
    return s_mic_cmd_queue;
}
