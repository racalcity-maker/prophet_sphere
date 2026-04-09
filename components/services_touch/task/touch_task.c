#include "touch_task.h"

#include <inttypes.h>
#include "driver/touch_sensor.h"
#include "app_events.h"
#include "app_tasking.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "sdkconfig.h"
#include "touch_service.h"

static const char *TAG = LOG_TAG_TOUCH;
static TaskHandle_t s_touch_task_handle;
static volatile bool s_stop_requested;
static EventGroupHandle_t s_lifecycle_events;
static portMUX_TYPE s_runtime_cfg_lock = portMUX_INITIALIZER_UNLOCKED;
static touch_runtime_config_t s_runtime_cfg;
static bool s_runtime_cfg_initialized;
static volatile bool s_recalibrate_requested;
static uint32_t s_threshold_floor[TOUCH_ZONE_COUNT];

#define TOUCH_CAL_WARMUP_SAMPLES 24U
#define TOUCH_CAL_MAX_SAMPLE_RETRIES 6U
#define TOUCH_CAL_NOISE_MULTIPLIER 4U
#define TOUCH_CAL_NOISE_MARGIN 20U
#ifndef CONFIG_ORB_TOUCH_STOP_TIMEOUT_MS
#define CONFIG_ORB_TOUCH_STOP_TIMEOUT_MS 1200
#endif

#define TOUCH_TASK_LIFECYCLE_STOP_REQUESTED BIT0
#define TOUCH_TASK_LIFECYCLE_STOPPED BIT1

typedef enum {
    TOUCH_TASK_STOP_PHASE_REQUESTED = 0,
    TOUCH_TASK_STOP_PHASE_WAITING,
    TOUCH_TASK_STOP_PHASE_COMPLETED,
    TOUCH_TASK_STOP_PHASE_TIMEOUT,
} touch_task_stop_phase_t;

static void touch_task_log_stop_phase(touch_task_stop_phase_t phase)
{
    switch (phase) {
    case TOUCH_TASK_STOP_PHASE_REQUESTED:
        ESP_LOGI(TAG, "touch_task stop: requested");
        break;
    case TOUCH_TASK_STOP_PHASE_WAITING:
        ESP_LOGI(TAG, "touch_task stop: waiting");
        break;
    case TOUCH_TASK_STOP_PHASE_COMPLETED:
        ESP_LOGI(TAG, "touch_task stop: completed");
        break;
    case TOUCH_TASK_STOP_PHASE_TIMEOUT:
        ESP_LOGW(TAG, "touch_task stop: timeout");
        break;
    default:
        break;
    }
}

static uint32_t clamp_u32_range(uint32_t value, uint32_t min_value, uint32_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static void touch_runtime_config_set_defaults(touch_runtime_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->poll_period_ms = (uint32_t)CONFIG_ORB_TOUCH_POLL_PERIOD_MS;
    cfg->calibration_samples = (uint32_t)CONFIG_ORB_TOUCH_CALIBRATION_SAMPLES;
    cfg->debounce_count = (uint8_t)CONFIG_ORB_TOUCH_DEBOUNCE_COUNT;
    cfg->release_debounce_count = (uint8_t)CONFIG_ORB_TOUCH_RELEASE_DEBOUNCE_COUNT;
#if CONFIG_ORB_TOUCH_HOLD_EVENT_ENABLE
    cfg->hold_event_enabled = true;
#else
    cfg->hold_event_enabled = false;
#endif
    cfg->hold_time_ms = (uint32_t)CONFIG_ORB_TOUCH_HOLD_TIME_MS;
    cfg->threshold_percent = (uint32_t)CONFIG_ORB_TOUCH_THRESHOLD_PERCENT;
    cfg->threshold_min_delta = (uint32_t)CONFIG_ORB_TOUCH_THRESHOLD_MIN_DELTA;
    cfg->release_threshold_percent = (uint32_t)CONFIG_ORB_TOUCH_RELEASE_THRESHOLD_PERCENT;
    cfg->filter_smooth_shift = (uint32_t)CONFIG_ORB_TOUCH_FILTER_SMOOTH_SHIFT;
    cfg->baseline_smooth_shift = (uint32_t)CONFIG_ORB_TOUCH_BASELINE_SMOOTH_SHIFT;
    cfg->stuck_press_timeout_ms = (uint32_t)CONFIG_ORB_TOUCH_STUCK_PRESS_TIMEOUT_MS;
}

static void touch_runtime_config_sanitize(touch_runtime_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->poll_period_ms = clamp_u32_range(cfg->poll_period_ms, 5U, 200U);
    cfg->calibration_samples = clamp_u32_range(cfg->calibration_samples, 8U, 200U);
    cfg->debounce_count = (uint8_t)clamp_u32_range((uint32_t)cfg->debounce_count, 1U, 20U);
    cfg->release_debounce_count = (uint8_t)clamp_u32_range((uint32_t)cfg->release_debounce_count, 1U, 20U);
    cfg->hold_time_ms = clamp_u32_range(cfg->hold_time_ms, 100U, 10000U);
    cfg->threshold_percent = clamp_u32_range(cfg->threshold_percent, 1U, 50U);
    cfg->threshold_min_delta = clamp_u32_range(cfg->threshold_min_delta, 10U, 5000U);
    cfg->release_threshold_percent = clamp_u32_range(cfg->release_threshold_percent, 10U, 95U);
    cfg->filter_smooth_shift = clamp_u32_range(cfg->filter_smooth_shift, 1U, 8U);
    cfg->baseline_smooth_shift = clamp_u32_range(cfg->baseline_smooth_shift, 2U, 10U);
    cfg->stuck_press_timeout_ms = clamp_u32_range(cfg->stuck_press_timeout_ms, 0U, 30000U);
}

static void touch_task_ensure_runtime_config_initialized(void)
{
    if (s_runtime_cfg_initialized) {
        return;
    }
    portENTER_CRITICAL(&s_runtime_cfg_lock);
    if (!s_runtime_cfg_initialized) {
        touch_runtime_config_set_defaults(&s_runtime_cfg);
        touch_runtime_config_sanitize(&s_runtime_cfg);
        s_runtime_cfg_initialized = true;
    }
    portEXIT_CRITICAL(&s_runtime_cfg_lock);
}

static touch_runtime_config_t touch_task_runtime_config_snapshot(void)
{
    touch_runtime_config_t cfg = { 0 };
    touch_task_ensure_runtime_config_initialized();
    portENTER_CRITICAL(&s_runtime_cfg_lock);
    cfg = s_runtime_cfg;
    portEXIT_CRITICAL(&s_runtime_cfg_lock);
    return cfg;
}

static void touch_task_request_stop_signal(void)
{
    s_stop_requested = true;
    if (s_lifecycle_events != NULL) {
        (void)xEventGroupSetBits(s_lifecycle_events, TOUCH_TASK_LIFECYCLE_STOP_REQUESTED);
    }
}

static bool touch_task_is_stop_requested(void)
{
    if (s_stop_requested) {
        return true;
    }
    EventGroupHandle_t events = s_lifecycle_events;
    if (events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(events);
    return ((bits & TOUCH_TASK_LIFECYCLE_STOP_REQUESTED) != 0U);
}

static void touch_task_wake(void)
{
    TaskHandle_t handle = s_touch_task_handle;
    if (handle != NULL) {
#if defined(INCLUDE_xTaskAbortDelay) && (INCLUDE_xTaskAbortDelay == 1)
        (void)xTaskAbortDelay(handle);
#else
        xTaskNotifyGive(handle);
#endif
    }
}

static esp_err_t touch_task_wait_stopped(TickType_t wait_ticks)
{
    if (wait_ticks == 0) {
        wait_ticks = 1;
    }
    if (s_lifecycle_events != NULL) {
        EventBits_t bits = xEventGroupWaitBits(s_lifecycle_events,
                                               TOUCH_TASK_LIFECYCLE_STOPPED,
                                               pdFALSE,
                                               pdFALSE,
                                               wait_ticks);
        if ((bits & TOUCH_TASK_LIFECYCLE_STOPPED) == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        return ESP_OK;
    }

    TickType_t deadline = xTaskGetTickCount() + wait_ticks;
    while (s_touch_task_handle != NULL && (int32_t)(xTaskGetTickCount() - deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return (s_touch_task_handle == NULL) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void touch_task_finalize_and_exit(void)
{
    if (s_lifecycle_events != NULL) {
        (void)xEventGroupSetBits(s_lifecycle_events, TOUCH_TASK_LIFECYCLE_STOPPED);
    }
    s_stop_requested = false;
    s_touch_task_handle = NULL;
    ESP_LOGI(TAG, "touch_task stopped");
    vTaskDelete(NULL);
}

void touch_task_load_default_runtime_config(touch_runtime_config_t *out_config)
{
    touch_runtime_config_set_defaults(out_config);
    touch_runtime_config_sanitize(out_config);
}

esp_err_t touch_task_get_runtime_config(touch_runtime_config_t *out_config)
{
    if (out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_config = touch_task_runtime_config_snapshot();
    return ESP_OK;
}

esp_err_t touch_task_apply_runtime_config(const touch_runtime_config_t *config, bool recalibrate_now)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    touch_runtime_config_t cfg = *config;
    touch_runtime_config_sanitize(&cfg);

    portENTER_CRITICAL(&s_runtime_cfg_lock);
    s_runtime_cfg = cfg;
    s_runtime_cfg_initialized = true;
    if (recalibrate_now) {
        s_recalibrate_requested = true;
    }
    portEXIT_CRITICAL(&s_runtime_cfg_lock);

    if (recalibrate_now) {
        touch_task_wake();
    }
    return ESP_OK;
}

static TickType_t poll_ticks(void)
{
    touch_runtime_config_t cfg = touch_task_runtime_config_snapshot();
    TickType_t ticks = pdMS_TO_TICKS(cfg.poll_period_ms);
    return ticks > 0 ? ticks : 1;
}

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

static void touch_task_sleep_until_next_poll(TickType_t *last_wake)
{
#if defined(INCLUDE_xTaskAbortDelay) && (INCLUDE_xTaskAbortDelay == 1)
    if (last_wake != NULL) {
        vTaskDelayUntil(last_wake, poll_ticks());
    } else {
        vTaskDelay(poll_ticks());
    }
#else
    (void)last_wake;
    (void)ulTaskNotifyTake(pdTRUE, poll_ticks());
#endif
}

static void touch_task_sleep_ms(uint32_t delay_ms)
{
#if defined(INCLUDE_xTaskAbortDelay) && (INCLUDE_xTaskAbortDelay == 1)
    vTaskDelay(ms_to_ticks_min1(delay_ms));
#else
    (void)ulTaskNotifyTake(pdTRUE, ms_to_ticks_min1(delay_ms));
#endif
}

static uint32_t tick_to_ms(TickType_t tick)
{
    return (uint32_t)(tick * portTICK_PERIOD_MS);
}

static uint16_t clamp_u16(uint32_t value)
{
    return (uint16_t)(value > UINT16_MAX ? UINT16_MAX : value);
}

static uint32_t iir_u32(uint32_t prev, uint32_t sample, uint32_t shift)
{
    if (prev == 0 || shift == 0) {
        return sample;
    }
    int32_t delta = (int32_t)sample - (int32_t)prev;
    return (uint32_t)((int32_t)prev + (delta >> shift));
}

static uint32_t abs_diff_u32(uint32_t a, uint32_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return (a >= b) ? a : b;
}

static uint32_t threshold_from_baseline(uint32_t baseline, const touch_runtime_config_t *cfg)
{
    uint32_t percent_term = (baseline * cfg->threshold_percent) / 100U;
    uint32_t min_term = cfg->threshold_min_delta;
    return max_u32(percent_term, min_term);
}

static uint32_t zone_touch_threshold(const touch_zone_runtime_t *zone, const touch_runtime_config_t *cfg)
{
    uint32_t threshold = threshold_from_baseline(zone->baseline, cfg);
    uint8_t idx = (uint8_t)zone->id;
    if (idx < TOUCH_ZONE_COUNT) {
        threshold = max_u32(threshold, s_threshold_floor[idx]);
    }
    return threshold;
}

static uint32_t zone_release_threshold(const touch_zone_runtime_t *zone, const touch_runtime_config_t *cfg)
{
    uint32_t touch_threshold = zone_touch_threshold(zone, cfg);
    uint32_t release_threshold = (touch_threshold * cfg->release_threshold_percent) / 100U;
    uint32_t min_release = cfg->threshold_min_delta / 3U;

    if (min_release < 10U) {
        min_release = 10U;
    }
    if (release_threshold < min_release) {
        release_threshold = min_release;
    }
    if (touch_threshold > 1U && release_threshold >= touch_threshold) {
        release_threshold = touch_threshold - 1U;
    }
    return release_threshold;
}

static esp_err_t read_zone_raw(touch_zone_runtime_t *zone)
{
    uint32_t raw = 0;
    esp_err_t err = touch_pad_read_raw_data((touch_pad_t)zone->channel, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch read failed ch=%d err=%s", (int)zone->channel, esp_err_to_name(err));
        return err;
    }
    zone->raw = raw;
    return ESP_OK;
}

static void post_touch_event(app_event_id_t event_id, const touch_zone_runtime_t *zone)
{
    app_event_t event = { 0 };
    event.id = event_id;
    event.source = APP_EVENT_SOURCE_TOUCH;
    event.timestamp_ms = tick_to_ms(xTaskGetTickCount());
    event.payload.touch.zone_id = (uint8_t)zone->id;
    event.payload.touch.strength = clamp_u16(zone->delta);
    event.payload.touch.raw = zone->raw;
    event.payload.touch.baseline = zone->baseline;
    event.payload.touch.delta = zone->delta;

    if (app_tasking_post_event(&event, CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "failed posting touch event id=%s zone=%u", app_event_id_to_str(event_id), zone->id);
        return;
    }

    if (event_id == APP_EVENT_TOUCH_DOWN) {
        ESP_LOGI(TAG,
                 "TOUCH_DOWN zone=%u raw=%" PRIu32 " base=%" PRIu32 " delta=%" PRIu32 " th=%" PRIu32,
                 zone->id,
                 zone->raw,
                 zone->baseline,
                 zone->delta,
                 zone->threshold);
    } else if (event_id == APP_EVENT_TOUCH_UP) {
        ESP_LOGI(TAG,
                 "TOUCH_UP zone=%u raw=%" PRIu32 " base=%" PRIu32 " delta=%" PRIu32,
                 zone->id,
                 zone->raw,
                 zone->baseline,
                 zone->delta);
    }
}

static void update_zone_metrics(touch_zone_runtime_t *zone, bool lock_baseline, const touch_runtime_config_t *cfg)
{
    if (read_zone_raw(zone) != ESP_OK) {
        return;
    }

    zone->filtered = iir_u32(zone->filtered, zone->raw, cfg->filter_smooth_shift);
    zone->threshold = zone_touch_threshold(zone, cfg);

    uint32_t pre_delta = abs_diff_u32(zone->baseline, zone->filtered);
    uint32_t baseline_track_limit = zone->threshold;
    if (baseline_track_limit < 8U) {
        baseline_track_limit = 8U;
    }

    if (!lock_baseline && pre_delta <= baseline_track_limit) {
        zone->baseline = iir_u32(zone->baseline, zone->filtered, cfg->baseline_smooth_shift);
    }

    zone->delta = abs_diff_u32(zone->baseline, zone->filtered);
    zone->threshold = zone_touch_threshold(zone, cfg);
    zone->state = zone->pressed ? TOUCH_STATE_ACTIVE : TOUCH_STATE_IDLE;
}

static esp_err_t read_all_zones_once(touch_runtime_status_t *status)
{
    for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(read_zone_raw(&status->zones[i]), TAG, "touch read failed in batch");
    }
    return ESP_OK;
}

static esp_err_t calibrate_zones(touch_runtime_status_t *status, const touch_runtime_config_t *cfg)
{
    uint64_t sums[TOUCH_ZONE_COUNT] = { 0 };
    uint32_t mins[TOUCH_ZONE_COUNT] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    uint32_t maxs[TOUCH_ZONE_COUNT] = { 0 };
    uint32_t samples = cfg->calibration_samples;

    for (uint32_t i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        s_threshold_floor[i] = cfg->threshold_min_delta;
    }

    for (uint32_t n = 0; n < TOUCH_CAL_WARMUP_SAMPLES; ++n) {
        if (touch_task_is_stop_requested()) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_RETURN_ON_ERROR(read_all_zones_once(status), TAG, "touch warmup read failed");
        touch_task_sleep_until_next_poll(NULL);
    }

    uint32_t valid_samples = 0;
    uint32_t retries = 0;
    uint32_t max_retries = samples * TOUCH_CAL_MAX_SAMPLE_RETRIES;
    while (valid_samples < samples) {
        if (touch_task_is_stop_requested()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (read_all_zones_once(status) != ESP_OK) {
            if (++retries > max_retries) {
                return ESP_FAIL;
            }
            touch_task_sleep_until_next_poll(NULL);
            continue;
        }

        bool sample_valid = true;
        for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
            uint32_t raw = status->zones[i].raw;
            if (raw == 0U) {
                sample_valid = false;
                break;
            }
        }
        if (!sample_valid) {
            if (++retries > max_retries) {
                return ESP_FAIL;
            }
            touch_task_sleep_until_next_poll(NULL);
            continue;
        }

        for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
            uint32_t raw = status->zones[i].raw;
            sums[i] += raw;
            if (raw < mins[i]) {
                mins[i] = raw;
            }
            if (raw > maxs[i]) {
                maxs[i] = raw;
            }
        }

        ++valid_samples;
        touch_task_sleep_until_next_poll(NULL);
    }

    for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        touch_zone_runtime_t *zone = &status->zones[i];
        uint32_t baseline = (uint32_t)(sums[i] / samples);
        uint32_t noise_span = (maxs[i] >= mins[i]) ? (maxs[i] - mins[i]) : 0U;
        uint32_t threshold_floor = (noise_span * TOUCH_CAL_NOISE_MULTIPLIER) + TOUCH_CAL_NOISE_MARGIN;
        uint32_t min_threshold = cfg->threshold_min_delta;
        if (threshold_floor < min_threshold) {
            threshold_floor = min_threshold;
        }
        s_threshold_floor[i] = threshold_floor;

        zone->raw = baseline;
        zone->filtered = baseline;
        zone->baseline = baseline;
        zone->delta = 0;
        zone->threshold = zone_touch_threshold(zone, cfg);
        zone->state = TOUCH_STATE_IDLE;
        zone->pressed = false;
        zone->hold_sent = false;
        zone->touch_count = 0;
        zone->release_count = 0;
        zone->pressed_since_ms = 0;

        ESP_LOGI(TAG,
                 "cal z%d baseline=%" PRIu32 " span=%" PRIu32 " th=%" PRIu32,
                 i,
                 baseline,
                 noise_span,
                 zone->threshold);
    }

    ESP_LOGI(TAG, "touch calibration complete");
    return ESP_OK;
}

static void maybe_log_diag(const touch_runtime_status_t *status)
{
#if CONFIG_ORB_TOUCH_DIAG_LOG_ENABLE
    static uint32_t last_diag_ms = 0;
    uint32_t now_ms = tick_to_ms(xTaskGetTickCount());
    if ((now_ms - last_diag_ms) < (uint32_t)CONFIG_ORB_TOUCH_DIAG_LOG_PERIOD_MS) {
        return;
    }
    last_diag_ms = now_ms;
    ESP_LOGI(TAG,
             "diag z0(d=%" PRIu32 " t=%" PRIu32 " p=%d) z1(d=%" PRIu32 " t=%" PRIu32 " p=%d) "
             "z2(d=%" PRIu32 " t=%" PRIu32 " p=%d) z3(d=%" PRIu32 " t=%" PRIu32 " p=%d)",
             status->zones[0].delta,
             status->zones[0].threshold,
             status->zones[0].pressed,
             status->zones[1].delta,
             status->zones[1].threshold,
             status->zones[1].pressed,
             status->zones[2].delta,
             status->zones[2].threshold,
             status->zones[2].pressed,
             status->zones[3].delta,
             status->zones[3].threshold,
             status->zones[3].pressed);
#else
    (void)status;
#endif
}

static void process_zone(touch_zone_runtime_t *zone, uint32_t now_ms, const touch_runtime_config_t *cfg)
{
    uint32_t touch_threshold = zone->threshold;
    uint32_t release_threshold = zone_release_threshold(zone, cfg);

    if (!zone->pressed) {
        if (zone->delta >= touch_threshold) {
            if (zone->touch_count < UINT8_MAX) {
                zone->touch_count++;
            }
        } else {
            zone->touch_count = 0;
        }
        zone->release_count = 0;

        if (zone->touch_count >= cfg->debounce_count) {
            zone->pressed = true;
            zone->state = TOUCH_STATE_ACTIVE;
            zone->hold_sent = false;
            zone->pressed_since_ms = now_ms;
            zone->touch_count = 0;
            zone->release_count = 0;
            post_touch_event(APP_EVENT_TOUCH_DOWN, zone);
        }
        return;
    }

    bool release_candidate = zone->delta <= release_threshold;
    if (release_candidate) {
        if (zone->release_count < UINT8_MAX) {
            zone->release_count++;
        }
    } else {
        zone->release_count = 0;
    }
    zone->touch_count = 0;

    if (zone->release_count >= cfg->release_debounce_count) {
        post_touch_event(APP_EVENT_TOUCH_UP, zone);
        zone->pressed = false;
        zone->state = TOUCH_STATE_IDLE;
        zone->hold_sent = false;
        zone->release_count = 0;
        zone->pressed_since_ms = 0;
        zone->baseline = iir_u32(zone->baseline, zone->filtered, 2);
        zone->threshold = zone_touch_threshold(zone, cfg);
        zone->delta = abs_diff_u32(zone->baseline, zone->filtered);
        return;
    }

    if (cfg->hold_event_enabled && !zone->hold_sent && (now_ms - zone->pressed_since_ms) >= cfg->hold_time_ms) {
        post_touch_event(APP_EVENT_TOUCH_HOLD, zone);
        zone->hold_sent = true;
    }

    if (cfg->stuck_press_timeout_ms > 0U && (now_ms - zone->pressed_since_ms) >= cfg->stuck_press_timeout_ms) {
        ESP_LOGW(TAG, "touch recovery: force release zone=%u", zone->id);
        post_touch_event(APP_EVENT_TOUCH_UP, zone);
        zone->pressed = false;
        zone->state = TOUCH_STATE_IDLE;
        zone->hold_sent = false;
        zone->touch_count = 0;
        zone->release_count = 0;
        zone->pressed_since_ms = 0;
    }
}

static void touch_task_entry(void *arg)
{
    (void)arg;

    if (!touch_service_real_touch_enabled()) {
        ESP_LOGW(TAG, "real touch disabled; no touch events will be produced");
        while (!touch_task_is_stop_requested()) {
            touch_task_sleep_ms(1000U);
        }
        touch_task_finalize_and_exit();
        return;
    }

    touch_runtime_status_t status = { 0 };
    touch_hw_channel_t channels[TOUCH_ZONE_COUNT] = { 0 };
    if (touch_service_get_zone_channels(channels) != ESP_OK) {
        ESP_LOGE(TAG, "failed to get touch channels");
        touch_task_finalize_and_exit();
        return;
    }

    for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        status.zones[i].id = (touch_zone_id_t)i;
        status.zones[i].channel = channels[i];
    }

    touch_runtime_config_t runtime_cfg = touch_task_runtime_config_snapshot();
    if (calibrate_zones(&status, &runtime_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "touch calibration failed");
        touch_task_finalize_and_exit();
        return;
    }

    TickType_t last_wake = xTaskGetTickCount();
    while (!touch_task_is_stop_requested()) {
        runtime_cfg = touch_task_runtime_config_snapshot();
        uint32_t now_ms = tick_to_ms(xTaskGetTickCount());

        for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
            touch_zone_runtime_t *zone = &status.zones[i];
            update_zone_metrics(zone, zone->pressed, &runtime_cfg);
            process_zone(zone, now_ms, &runtime_cfg);
        }

        if (s_recalibrate_requested) {
            s_recalibrate_requested = false;
            runtime_cfg = touch_task_runtime_config_snapshot();
            if (calibrate_zones(&status, &runtime_cfg) != ESP_OK) {
                ESP_LOGW(TAG, "touch recalibration failed");
            } else {
                ESP_LOGI(TAG, "touch recalibration applied");
            }
        }

        maybe_log_diag(&status);
        touch_task_sleep_until_next_poll(&last_wake);
    }

    touch_task_finalize_and_exit();
}

esp_err_t touch_task_start(void)
{
    if (s_touch_task_handle != NULL) {
        return ESP_OK;
    }
    if (s_lifecycle_events == NULL) {
        s_lifecycle_events = xEventGroupCreate();
        if (s_lifecycle_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    touch_task_ensure_runtime_config_initialized();
    s_stop_requested = false;
    s_recalibrate_requested = false;
    (void)xEventGroupClearBits(s_lifecycle_events, TOUCH_TASK_LIFECYCLE_STOP_REQUESTED | TOUCH_TASK_LIFECYCLE_STOPPED);

    BaseType_t ok = xTaskCreate(touch_task_entry,
                                "touch_task",
                                CONFIG_ORB_TOUCH_TASK_STACK_SIZE,
                                NULL,
                                CONFIG_ORB_TOUCH_TASK_PRIORITY,
                                &s_touch_task_handle);
    if (ok != pdPASS) {
        s_touch_task_handle = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t touch_task_stop(void)
{
    if (s_touch_task_handle == NULL) {
        return ESP_OK;
    }

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (self == s_touch_task_handle) {
        touch_task_request_stop_signal();
        return ESP_OK;
    }

    touch_task_log_stop_phase(TOUCH_TASK_STOP_PHASE_REQUESTED);
    touch_task_request_stop_signal();
    if (s_lifecycle_events != NULL) {
        (void)xEventGroupClearBits(s_lifecycle_events, TOUCH_TASK_LIFECYCLE_STOPPED);
    }
    touch_task_wake();

    touch_task_log_stop_phase(TOUCH_TASK_STOP_PHASE_WAITING);
    TickType_t stop_wait_ticks = pdMS_TO_TICKS((uint32_t)CONFIG_ORB_TOUCH_STOP_TIMEOUT_MS);
    esp_err_t wait_err = touch_task_wait_stopped(stop_wait_ticks);
    if (wait_err != ESP_OK) {
        touch_task_log_stop_phase(TOUCH_TASK_STOP_PHASE_TIMEOUT);
        return wait_err;
    }

    touch_task_log_stop_phase(TOUCH_TASK_STOP_PHASE_COMPLETED);
    return ESP_OK;
}
