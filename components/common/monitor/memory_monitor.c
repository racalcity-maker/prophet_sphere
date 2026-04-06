#include "memory_monitor.h"

#include <inttypes.h>
#include <stdbool.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "log_tags.h"

#ifndef CONFIG_ORB_MEM_MONITOR_ENABLE
#define CONFIG_ORB_MEM_MONITOR_ENABLE 0
#endif

#ifndef CONFIG_ORB_MEM_MONITOR_PERIOD_MS
#define CONFIG_ORB_MEM_MONITOR_PERIOD_MS 60000
#endif

static const char *TAG = LOG_TAG_MEM_MON;

#if CONFIG_ORB_MEM_MONITOR_ENABLE
static esp_timer_handle_t s_mem_timer;
static bool s_started;

static void memory_monitor_log_snapshot(void)
{
    const uint32_t free_total = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t largest_total = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    const uint32_t free_sram =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largest_sram =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    const uint32_t free_psram =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const uint32_t largest_psram =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
             "heap free/largest (bytes): total=%" PRIu32 "/%" PRIu32
             " sram=%" PRIu32 "/%" PRIu32 " psram=%" PRIu32 "/%" PRIu32,
             free_total,
             largest_total,
             free_sram,
             largest_sram,
             free_psram,
             largest_psram);
}

static void memory_monitor_timer_cb(void *arg)
{
    (void)arg;
    memory_monitor_log_snapshot();
}
#endif

esp_err_t memory_monitor_start(void)
{
#if !CONFIG_ORB_MEM_MONITOR_ENABLE
    return ESP_OK;
#else
    if (s_started) {
        return ESP_OK;
    }

    uint64_t period_us = (uint64_t)CONFIG_ORB_MEM_MONITOR_PERIOD_MS * 1000ULL;
    if (period_us == 0) {
        period_us = 60000000ULL;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = memory_monitor_timer_cb,
        .arg = NULL,
        .name = "orb_mem_mon",
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_mem_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_timer_start_periodic(s_mem_timer, period_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        (void)esp_timer_delete(s_mem_timer);
        s_mem_timer = NULL;
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "memory monitor started: period=%" PRIu64 "ms", period_us / 1000ULL);
    memory_monitor_log_snapshot();
    return ESP_OK;
#endif
}
