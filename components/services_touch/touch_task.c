#include "touch_task.h"

#include <inttypes.h>
#include "driver/touch_sensor.h"
#include "app_events.h"
#include "app_tasking.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"
#include "sdkconfig.h"
#include "touch_service.h"

static const char *TAG = LOG_TAG_TOUCH;
static TaskHandle_t s_touch_task_handle;
static uint32_t s_threshold_floor[TOUCH_ZONE_COUNT];

#define TOUCH_CAL_WARMUP_SAMPLES 24U
#define TOUCH_CAL_MAX_SAMPLE_RETRIES 6U
#define TOUCH_CAL_NOISE_MULTIPLIER 4U
#define TOUCH_CAL_NOISE_MARGIN 20U

static TickType_t poll_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_TOUCH_POLL_PERIOD_MS);
    return ticks > 0 ? ticks : 1;
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

static uint32_t threshold_from_baseline(uint32_t baseline)
{
    uint32_t percent_term = (baseline * (uint32_t)CONFIG_ORB_TOUCH_THRESHOLD_PERCENT) / 100U;
    uint32_t min_term = (uint32_t)CONFIG_ORB_TOUCH_THRESHOLD_MIN_DELTA;
    return max_u32(percent_term, min_term);
}

static uint32_t zone_touch_threshold(const touch_zone_runtime_t *zone)
{
    uint32_t threshold = threshold_from_baseline(zone->baseline);
    uint8_t idx = (uint8_t)zone->id;
    if (idx < TOUCH_ZONE_COUNT) {
        threshold = max_u32(threshold, s_threshold_floor[idx]);
    }
    return threshold;
}

static uint32_t zone_release_threshold(const touch_zone_runtime_t *zone)
{
    uint32_t touch_threshold = zone_touch_threshold(zone);
    uint32_t release_threshold = (touch_threshold * (uint32_t)CONFIG_ORB_TOUCH_RELEASE_THRESHOLD_PERCENT) / 100U;
    uint32_t min_release = (uint32_t)CONFIG_ORB_TOUCH_THRESHOLD_MIN_DELTA / 3U;

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

static void update_zone_metrics(touch_zone_runtime_t *zone, bool lock_baseline)
{
    if (read_zone_raw(zone) != ESP_OK) {
        return;
    }

    zone->filtered = iir_u32(zone->filtered, zone->raw, CONFIG_ORB_TOUCH_FILTER_SMOOTH_SHIFT);
    zone->threshold = zone_touch_threshold(zone);

    uint32_t pre_delta = abs_diff_u32(zone->baseline, zone->filtered);
    uint32_t baseline_track_limit = zone->threshold;
    if (baseline_track_limit < 8U) {
        baseline_track_limit = 8U;
    }

    if (!lock_baseline && pre_delta <= baseline_track_limit) {
        zone->baseline = iir_u32(zone->baseline, zone->filtered, CONFIG_ORB_TOUCH_BASELINE_SMOOTH_SHIFT);
    }

    zone->delta = abs_diff_u32(zone->baseline, zone->filtered);
    zone->threshold = zone_touch_threshold(zone);
    zone->state = zone->pressed ? TOUCH_STATE_ACTIVE : TOUCH_STATE_IDLE;
}

static esp_err_t read_all_zones_once(touch_runtime_status_t *status)
{
    for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(read_zone_raw(&status->zones[i]), TAG, "touch read failed in batch");
    }
    return ESP_OK;
}

static esp_err_t calibrate_zones(touch_runtime_status_t *status)
{
    uint64_t sums[TOUCH_ZONE_COUNT] = { 0 };
    uint32_t mins[TOUCH_ZONE_COUNT] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    uint32_t maxs[TOUCH_ZONE_COUNT] = { 0 };
    uint32_t samples = (uint32_t)CONFIG_ORB_TOUCH_CALIBRATION_SAMPLES;

    for (uint32_t i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        s_threshold_floor[i] = (uint32_t)CONFIG_ORB_TOUCH_THRESHOLD_MIN_DELTA;
    }

    for (uint32_t n = 0; n < TOUCH_CAL_WARMUP_SAMPLES; ++n) {
        ESP_RETURN_ON_ERROR(read_all_zones_once(status), TAG, "touch warmup read failed");
        vTaskDelay(poll_ticks());
    }

    uint32_t valid_samples = 0;
    uint32_t retries = 0;
    uint32_t max_retries = samples * TOUCH_CAL_MAX_SAMPLE_RETRIES;
    while (valid_samples < samples) {
        if (read_all_zones_once(status) != ESP_OK) {
            if (++retries > max_retries) {
                return ESP_FAIL;
            }
            vTaskDelay(poll_ticks());
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
            vTaskDelay(poll_ticks());
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
        vTaskDelay(poll_ticks());
    }

    for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        touch_zone_runtime_t *zone = &status->zones[i];
        uint32_t baseline = (uint32_t)(sums[i] / samples);
        uint32_t noise_span = (maxs[i] >= mins[i]) ? (maxs[i] - mins[i]) : 0U;
        uint32_t threshold_floor = (noise_span * TOUCH_CAL_NOISE_MULTIPLIER) + TOUCH_CAL_NOISE_MARGIN;
        uint32_t min_threshold = (uint32_t)CONFIG_ORB_TOUCH_THRESHOLD_MIN_DELTA;
        if (threshold_floor < min_threshold) {
            threshold_floor = min_threshold;
        }
        s_threshold_floor[i] = threshold_floor;

        zone->raw = baseline;
        zone->filtered = baseline;
        zone->baseline = baseline;
        zone->delta = 0;
        zone->threshold = zone_touch_threshold(zone);
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

static void process_zone(touch_zone_runtime_t *zone, uint32_t now_ms)
{
    uint32_t touch_threshold = zone->threshold;
    uint32_t release_threshold = zone_release_threshold(zone);

    if (!zone->pressed) {
        if (zone->delta >= touch_threshold) {
            if (zone->touch_count < UINT8_MAX) {
                zone->touch_count++;
            }
        } else {
            zone->touch_count = 0;
        }
        zone->release_count = 0;

        if (zone->touch_count >= (uint8_t)CONFIG_ORB_TOUCH_DEBOUNCE_COUNT) {
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

    if (zone->release_count >= (uint8_t)CONFIG_ORB_TOUCH_RELEASE_DEBOUNCE_COUNT) {
        post_touch_event(APP_EVENT_TOUCH_UP, zone);
        zone->pressed = false;
        zone->state = TOUCH_STATE_IDLE;
        zone->hold_sent = false;
        zone->release_count = 0;
        zone->pressed_since_ms = 0;
        zone->baseline = iir_u32(zone->baseline, zone->filtered, 2);
        zone->threshold = zone_touch_threshold(zone);
        zone->delta = abs_diff_u32(zone->baseline, zone->filtered);
        return;
    }

#if CONFIG_ORB_TOUCH_HOLD_EVENT_ENABLE
    if (!zone->hold_sent && (now_ms - zone->pressed_since_ms) >= (uint32_t)CONFIG_ORB_TOUCH_HOLD_TIME_MS) {
        post_touch_event(APP_EVENT_TOUCH_HOLD, zone);
        zone->hold_sent = true;
    }
#endif

#if CONFIG_ORB_TOUCH_STUCK_PRESS_TIMEOUT_MS > 0
    if ((now_ms - zone->pressed_since_ms) >= (uint32_t)CONFIG_ORB_TOUCH_STUCK_PRESS_TIMEOUT_MS) {
        ESP_LOGW(TAG, "touch recovery: force release zone=%u", zone->id);
        post_touch_event(APP_EVENT_TOUCH_UP, zone);
        zone->pressed = false;
        zone->state = TOUCH_STATE_IDLE;
        zone->hold_sent = false;
        zone->touch_count = 0;
        zone->release_count = 0;
        zone->pressed_since_ms = 0;
    }
#endif
}

static void touch_task_entry(void *arg)
{
    (void)arg;

    if (!touch_service_real_touch_enabled()) {
        ESP_LOGW(TAG, "real touch disabled; no touch events will be produced");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    touch_runtime_status_t status = { 0 };
    touch_hw_channel_t channels[TOUCH_ZONE_COUNT] = { 0 };
    if (touch_service_get_zone_channels(channels) != ESP_OK) {
        ESP_LOGE(TAG, "failed to get touch channels");
        s_touch_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        status.zones[i].id = (touch_zone_id_t)i;
        status.zones[i].channel = channels[i];
    }

    if (calibrate_zones(&status) != ESP_OK) {
        ESP_LOGE(TAG, "touch calibration failed");
        s_touch_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        uint32_t now_ms = tick_to_ms(xTaskGetTickCount());

        for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
            touch_zone_runtime_t *zone = &status.zones[i];
            update_zone_metrics(zone, zone->pressed);
            process_zone(zone, now_ms);
        }

        maybe_log_diag(&status);
        vTaskDelayUntil(&last_wake, poll_ticks());
    }
}

esp_err_t touch_task_start(void)
{
    if (s_touch_task_handle != NULL) {
        return ESP_OK;
    }

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

    TaskHandle_t handle = s_touch_task_handle;
    s_touch_task_handle = NULL;
    vTaskDelete(handle);
    ESP_LOGI(TAG, "touch_task stopped");
    return ESP_OK;
}
