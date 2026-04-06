#include "interaction_sequence.h"

#include <inttypes.h>
#include "app_tasking.h"
#include "control_dispatch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "log_tags.h"
#include "sdkconfig.h"

typedef enum {
    INTERACTION_SEQUENCE_IDLE = 0,
    INTERACTION_SEQUENCE_WAIT_FIRST_DONE,
    INTERACTION_SEQUENCE_WAIT_GAP_TIMER,
    INTERACTION_SEQUENCE_WAIT_SECOND_DONE,
} interaction_sequence_state_t;

typedef struct {
    interaction_sequence_state_t state;
    uint32_t first_asset_id;
    uint32_t second_asset_id;
    uint32_t gap_ms;
} interaction_sequence_ctx_t;

static const char *TAG = LOG_TAG_MODE_MANAGER;
static SemaphoreHandle_t s_seq_mutex;
static TimerHandle_t s_gap_timer;
static interaction_sequence_ctx_t s_ctx;
static interaction_sequence_before_second_hook_t s_before_second_hook;

static TickType_t lock_timeout_ticks(void)
{
    return pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

static TickType_t ms_to_timer_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

static esp_err_t lock_ctx(void)
{
    if (s_seq_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_seq_mutex, lock_timeout_ticks()) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void unlock_ctx(void)
{
    if (s_seq_mutex != NULL) {
        xSemaphoreGive(s_seq_mutex);
    }
}

static esp_err_t queue_audio_play(uint32_t asset_id)
{
    return control_dispatch_queue_audio_asset(asset_id);
}

static void gap_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    esp_err_t err = app_tasking_post_timer_event_reliable(APP_TIMER_KIND_AUDIO_SEQUENCE_GAP, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to post audio gap timer event: %s", esp_err_to_name(err));
    }
}

static esp_err_t ensure_gap_timer(void)
{
    if (s_gap_timer != NULL) {
        return ESP_OK;
    }
    s_gap_timer = xTimerCreate("audio_gap", ms_to_timer_ticks(1), pdFALSE, NULL, gap_timer_cb);
    if (s_gap_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t interaction_sequence_init(void)
{
    if (s_seq_mutex != NULL) {
        return ensure_gap_timer();
    }

    s_seq_mutex = xSemaphoreCreateMutex();
    if (s_seq_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_ctx = (interaction_sequence_ctx_t){ 0 };
    return ensure_gap_timer();
}

esp_err_t interaction_sequence_set_before_second_hook(interaction_sequence_before_second_hook_t hook)
{
    s_before_second_hook = hook;
    return ESP_OK;
}

esp_err_t interaction_sequence_reset(void)
{
    esp_err_t err = interaction_sequence_init();
    if (err != ESP_OK) {
        return err;
    }

    if (xTimerStop(s_gap_timer, lock_timeout_ticks()) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }

    err = lock_ctx();
    if (err != ESP_OK) {
        return err;
    }
    s_ctx = (interaction_sequence_ctx_t){ 0 };
    unlock_ctx();
    return ESP_OK;
}

esp_err_t interaction_sequence_start_two_track(uint32_t first_asset_id, uint32_t second_asset_id, uint32_t gap_ms)
{
    if (first_asset_id == 0U || second_asset_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(interaction_sequence_init(), TAG, "sequence init failed");

    esp_err_t err = lock_ctx();
    if (err != ESP_OK) {
        return err;
    }
    if (s_ctx.state != INTERACTION_SEQUENCE_IDLE) {
        unlock_ctx();
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.first_asset_id = first_asset_id;
    s_ctx.second_asset_id = second_asset_id;
    s_ctx.gap_ms = gap_ms;
    s_ctx.state = INTERACTION_SEQUENCE_WAIT_FIRST_DONE;
    unlock_ctx();

    err = queue_audio_play(first_asset_id);
    if (err != ESP_OK) {
        (void)interaction_sequence_reset();
        return err;
    }

    ESP_LOGI(TAG,
             "audio sequence started first=%" PRIu32 " second=%" PRIu32 " gap=%" PRIu32 "ms",
             first_asset_id,
             second_asset_id,
             gap_ms);
    return ESP_OK;
}

esp_err_t interaction_sequence_on_audio_done(uint32_t finished_asset_id, bool *consumed, bool *completed)
{
    if (consumed == NULL || completed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *consumed = false;
    *completed = false;

    esp_err_t err = lock_ctx();
    if (err != ESP_OK) {
        return err;
    }

    if (s_ctx.state == INTERACTION_SEQUENCE_WAIT_FIRST_DONE && finished_asset_id == s_ctx.first_asset_id) {
        uint32_t gap_ms = s_ctx.gap_ms;
        s_ctx.state = (gap_ms == 0U) ? INTERACTION_SEQUENCE_WAIT_SECOND_DONE : INTERACTION_SEQUENCE_WAIT_GAP_TIMER;
        uint32_t second_asset_id = s_ctx.second_asset_id;
        *consumed = true;
        unlock_ctx();

        if (s_before_second_hook != NULL) {
            err = s_before_second_hook(second_asset_id, gap_ms);
            if (err != ESP_OK) {
                (void)interaction_sequence_reset();
                return err;
            }
        }

        if (gap_ms == 0U) {
            err = queue_audio_play(second_asset_id);
            if (err != ESP_OK) {
                (void)interaction_sequence_reset();
                return err;
            }
            return ESP_OK;
        }

        if (xTimerStop(s_gap_timer, lock_timeout_ticks()) != pdPASS) {
            (void)interaction_sequence_reset();
            return ESP_ERR_TIMEOUT;
        }
        if (xTimerChangePeriod(s_gap_timer, ms_to_timer_ticks(gap_ms), lock_timeout_ticks()) != pdPASS) {
            (void)interaction_sequence_reset();
            return ESP_ERR_TIMEOUT;
        }
        if (xTimerStart(s_gap_timer, lock_timeout_ticks()) != pdPASS) {
            (void)interaction_sequence_reset();
            return ESP_ERR_TIMEOUT;
        }
        return ESP_OK;
    }

    if (s_ctx.state == INTERACTION_SEQUENCE_WAIT_SECOND_DONE && finished_asset_id == s_ctx.second_asset_id) {
        *consumed = true;
        *completed = true;
        s_ctx = (interaction_sequence_ctx_t){ 0 };
        unlock_ctx();
        return ESP_OK;
    }

    unlock_ctx();
    return ESP_OK;
}

esp_err_t interaction_sequence_on_timer_expired(app_timer_kind_t timer_kind, bool *consumed)
{
    if (consumed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *consumed = false;

    if (timer_kind != APP_TIMER_KIND_AUDIO_SEQUENCE_GAP) {
        return ESP_OK;
    }

    uint32_t second_asset_id = 0;
    esp_err_t err = lock_ctx();
    if (err != ESP_OK) {
        return err;
    }

    *consumed = true;
    if (s_ctx.state != INTERACTION_SEQUENCE_WAIT_GAP_TIMER) {
        unlock_ctx();
        return ESP_OK;
    }

    second_asset_id = s_ctx.second_asset_id;
    s_ctx.state = INTERACTION_SEQUENCE_WAIT_SECOND_DONE;
    unlock_ctx();

    err = queue_audio_play(second_asset_id);
    if (err != ESP_OK) {
        (void)interaction_sequence_reset();
        return err;
    }
    return ESP_OK;
}
