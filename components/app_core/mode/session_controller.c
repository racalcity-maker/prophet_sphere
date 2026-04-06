#include "session_controller.h"

#include <inttypes.h>
#include "app_defs.h"
#include "app_events.h"
#include "app_tasking.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "log_tags.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_SESSION_CTRL;
static SemaphoreHandle_t s_session_mutex;
static TimerHandle_t s_cooldown_timer;
static session_info_t s_session_info = {
    .session_id = ORB_INVALID_SESSION_ID,
    .state = SESSION_IDLE,
    .active = false,
};

static TickType_t lock_timeout_ticks(void)
{
    return pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
}

static esp_err_t lock_session(void)
{
    if (s_session_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_session_mutex, lock_timeout_ticks()) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void unlock_session(void)
{
    if (s_session_mutex != NULL) {
        xSemaphoreGive(s_session_mutex);
    }
}

static void update_active_flag(void)
{
    s_session_info.active = (s_session_info.state != SESSION_IDLE);
}

static void cooldown_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    esp_err_t err = app_tasking_post_timer_event_reliable(APP_TIMER_KIND_SESSION_COOLDOWN, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to post cooldown timer event: %s", esp_err_to_name(err));
    }
}

static esp_err_t ensure_cooldown_timer(void)
{
    if (s_cooldown_timer != NULL) {
        return ESP_OK;
    }

    s_cooldown_timer =
        xTimerCreate("sess_cooldown", pdMS_TO_TICKS(CONFIG_ORB_OFFLINE_COOLDOWN_MS), pdFALSE, NULL, cooldown_timer_cb);
    if (s_cooldown_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const char *session_controller_state_to_str(session_state_t state)
{
    switch (state) {
    case SESSION_IDLE:
        return "IDLE";
    case SESSION_ACTIVATING:
        return "ACTIVATING";
    case SESSION_SPEAKING:
        return "SPEAKING";
    case SESSION_COOLDOWN:
        return "COOLDOWN";
    default:
        return "UNKNOWN";
    }
}

esp_err_t session_controller_init(void)
{
    if (s_session_mutex != NULL) {
        return ESP_OK;
    }

    s_session_mutex = xSemaphoreCreateMutex();
    if (s_session_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ensure_cooldown_timer();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "session controller initialized");
    return ESP_OK;
}

esp_err_t session_controller_start_interaction(void)
{
    esp_err_t err = lock_session();
    if (err != ESP_OK) {
        return err;
    }

    if (s_session_info.state != SESSION_IDLE) {
        ESP_LOGW(TAG, "start ignored: state=%s", session_controller_state_to_str(s_session_info.state));
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }

    s_session_info.session_id = (uint32_t)xTaskGetTickCount();
    s_session_info.state = SESSION_ACTIVATING;
    update_active_flag();
    ESP_LOGI(TAG, "session start id=%" PRIu32 " state=%s", s_session_info.session_id, "ACTIVATING");

    unlock_session();
    return ESP_OK;
}

esp_err_t session_controller_mark_speaking(void)
{
    esp_err_t err = lock_session();
    if (err != ESP_OK) {
        return err;
    }

    if (s_session_info.state != SESSION_ACTIVATING) {
        ESP_LOGW(TAG, "mark speaking ignored: state=%s", session_controller_state_to_str(s_session_info.state));
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }

    s_session_info.state = SESSION_SPEAKING;
    update_active_flag();
    ESP_LOGI(TAG, "session id=%" PRIu32 " state=%s", s_session_info.session_id, "SPEAKING");

    unlock_session();
    return ESP_OK;
}

esp_err_t session_controller_begin_cooldown(uint32_t cooldown_ms)
{
    if (cooldown_ms == 0) {
        cooldown_ms = CONFIG_ORB_OFFLINE_COOLDOWN_MS;
    }

    esp_err_t err = ensure_cooldown_timer();
    if (err != ESP_OK) {
        return err;
    }

    err = lock_session();
    if (err != ESP_OK) {
        return err;
    }

    if (s_session_info.state != SESSION_SPEAKING && s_session_info.state != SESSION_COOLDOWN) {
        ESP_LOGW(TAG, "cooldown ignored: state=%s", session_controller_state_to_str(s_session_info.state));
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }
    unlock_session();

    if (xTimerStop(s_cooldown_timer, lock_timeout_ticks()) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    if (xTimerChangePeriod(s_cooldown_timer, pdMS_TO_TICKS(cooldown_ms), lock_timeout_ticks()) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    if (xTimerStart(s_cooldown_timer, lock_timeout_ticks()) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }

    err = lock_session();
    if (err != ESP_OK) {
        (void)xTimerStop(s_cooldown_timer, lock_timeout_ticks());
        return err;
    }
    if (s_session_info.state != SESSION_SPEAKING && s_session_info.state != SESSION_COOLDOWN) {
        unlock_session();
        (void)xTimerStop(s_cooldown_timer, lock_timeout_ticks());
        return ESP_ERR_INVALID_STATE;
    }

    s_session_info.state = SESSION_COOLDOWN;
    update_active_flag();
    ESP_LOGI(TAG,
             "session id=%" PRIu32 " state=%s duration=%" PRIu32 "ms",
             s_session_info.session_id,
             "COOLDOWN",
             cooldown_ms);
    unlock_session();

    return ESP_OK;
}

esp_err_t session_controller_finish_cooldown(void)
{
    esp_err_t err = lock_session();
    if (err != ESP_OK) {
        return err;
    }

    if (s_session_info.state != SESSION_COOLDOWN) {
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "session end id=%" PRIu32, s_session_info.session_id);
    s_session_info.session_id = ORB_INVALID_SESSION_ID;
    s_session_info.state = SESSION_IDLE;
    update_active_flag();
    ESP_LOGI(TAG, "session state=%s", "IDLE");

    unlock_session();
    return ESP_OK;
}

esp_err_t session_controller_reset_to_idle(void)
{
    esp_err_t err = lock_session();
    if (err != ESP_OK) {
        return err;
    }

    if (s_session_info.active) {
        ESP_LOGI(TAG,
                 "reset session id=%" PRIu32 " from state=%s",
                 s_session_info.session_id,
                 session_controller_state_to_str(s_session_info.state));
    }
    s_session_info.session_id = ORB_INVALID_SESSION_ID;
    s_session_info.state = SESSION_IDLE;
    update_active_flag();

    unlock_session();

    if (s_cooldown_timer != NULL) {
        (void)xTimerStop(s_cooldown_timer, lock_timeout_ticks());
    }
    return ESP_OK;
}

esp_err_t session_controller_get_info(session_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_session();
    if (err != ESP_OK) {
        return err;
    }

    *info = s_session_info;
    unlock_session();
    return ESP_OK;
}
