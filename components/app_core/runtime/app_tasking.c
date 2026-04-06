#include "app_tasking.h"

#include <stdbool.h>
#include "sdkconfig.h"
#include "app_control_task.h"
#include "app_fsm.h"
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
                      s_ai_cmd_queue != NULL && s_mic_cmd_queue != NULL);
    if (all_ready) {
        return ESP_OK;
    }

    if (s_app_event_queue != NULL || s_led_cmd_queue != NULL || s_audio_cmd_queue != NULL || s_ai_cmd_queue != NULL ||
        s_mic_cmd_queue != NULL) {
        ESP_LOGW(TAG, "partial queue state detected, cleaning up and retrying init");
        cleanup_queues();
    }

    QueueHandle_t app_q = xQueueCreate(CONFIG_ORB_APP_EVENT_QUEUE_LENGTH, sizeof(app_event_t));
    QueueHandle_t led_q = xQueueCreate(CONFIG_ORB_LED_COMMAND_QUEUE_LENGTH, sizeof(led_command_t));
    QueueHandle_t audio_q = create_audio_queue();
    QueueHandle_t ai_q = xQueueCreate(CONFIG_ORB_AI_COMMAND_QUEUE_LENGTH, sizeof(ai_command_t));
    QueueHandle_t mic_q = xQueueCreate(CONFIG_ORB_MIC_COMMAND_QUEUE_LENGTH, sizeof(mic_command_t));

    if (app_q == NULL || led_q == NULL || audio_q == NULL || ai_q == NULL || mic_q == NULL) {
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
        ESP_LOGE(TAG, "queue allocation failed");
        return ESP_ERR_NO_MEM;
    }

    s_app_event_queue = app_q;
    s_led_cmd_queue = led_q;
    s_audio_cmd_queue = audio_q;
    s_ai_cmd_queue = ai_q;
    s_mic_cmd_queue = mic_q;

    esp_err_t err = app_fsm_init();
    if (err != ESP_OK) {
        cleanup_queues();
        return err;
    }

    ESP_LOGI(TAG,
             "queues ready app=%u led=%u audio=%u ai=%u mic=%u",
             (unsigned)CONFIG_ORB_APP_EVENT_QUEUE_LENGTH,
             (unsigned)CONFIG_ORB_LED_COMMAND_QUEUE_LENGTH,
             (unsigned)CONFIG_ORB_AUDIO_COMMAND_QUEUE_LENGTH,
             (unsigned)CONFIG_ORB_AI_COMMAND_QUEUE_LENGTH,
             (unsigned)CONFIG_ORB_MIC_COMMAND_QUEUE_LENGTH);
    return ESP_OK;
}

esp_err_t app_tasking_start_app_control_task(void)
{
    if (s_app_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return app_control_task_start(s_app_event_queue);
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
    return send_to_queue(s_audio_cmd_queue, command, timeout_ms);
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
