#include "audio_reactive_analyzer.h"

#include "sdkconfig.h"

#ifndef CONFIG_ORB_AUDIO_REACTIVE_LEVEL_SCALE
#define CONFIG_ORB_AUDIO_REACTIVE_LEVEL_SCALE 7000
#endif
#ifndef CONFIG_ORB_AUDIO_REACTIVE_NOISE_GATE
#define CONFIG_ORB_AUDIO_REACTIVE_NOISE_GATE 5
#endif
#ifndef CONFIG_ORB_AUDIO_REACTIVE_MIN_SPAN
#define CONFIG_ORB_AUDIO_REACTIVE_MIN_SPAN 110
#endif

static uint32_t abs_i32(int32_t v)
{
    return (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
}

void audio_reactive_analyzer_init(audio_reactive_analyzer_t *analyzer)
{
    if (analyzer == NULL) {
        return;
    }
    analyzer->floor_metric = 0U;
    analyzer->ceiling_metric = 0U;
    analyzer->filtered_level = 0U;
    analyzer->prev_sample = 0;
    analyzer->prev_valid = 0U;
}

void audio_reactive_analyzer_reset(audio_reactive_analyzer_t *analyzer)
{
    audio_reactive_analyzer_init(analyzer);
}

void audio_reactive_analyzer_process_pcm16_mono(audio_reactive_analyzer_t *analyzer,
                                                const int16_t *samples,
                                                size_t sample_count)
{
    if (analyzer == NULL || samples == NULL || sample_count == 0U) {
        return;
    }

    uint64_t sum_abs = 0U;
    uint64_t sum_diff = 0U;
    uint32_t diff_count = 0U;

    for (size_t i = 0; i < sample_count; ++i) {
        int32_t sample = (int32_t)samples[i];
        sum_abs += abs_i32(sample);

        if (analyzer->prev_valid != 0U) {
            int32_t diff = sample - (int32_t)analyzer->prev_sample;
            sum_diff += abs_i32(diff);
            diff_count++;
        }

        analyzer->prev_sample = (int16_t)sample;
        analyzer->prev_valid = 1U;
    }

    uint32_t avg_abs = (uint32_t)(sum_abs / sample_count);
    uint32_t avg_diff = (diff_count > 0U) ? (uint32_t)(sum_diff / diff_count) : 0U;
    uint32_t metric_pcm = (avg_abs * 55U + avg_diff * 45U) / 100U;

    uint32_t scale = (uint32_t)CONFIG_ORB_AUDIO_REACTIVE_LEVEL_SCALE;
    if (scale == 0U) {
        scale = 1U;
    }
    uint32_t metric = (metric_pcm * 1000U) / scale;
    if (metric > 6000U) {
        metric = 6000U;
    }

    if (analyzer->ceiling_metric == 0U) {
        analyzer->floor_metric = metric;
        analyzer->ceiling_metric = metric + (uint32_t)CONFIG_ORB_AUDIO_REACTIVE_MIN_SPAN;
    }

    if (metric <= analyzer->floor_metric) {
        analyzer->floor_metric = (analyzer->floor_metric * 70U + metric * 30U) / 100U;
    } else {
        analyzer->floor_metric = (analyzer->floor_metric * 95U + metric * 5U) / 100U;
    }

    if (metric >= analyzer->ceiling_metric) {
        analyzer->ceiling_metric = (analyzer->ceiling_metric * 50U + metric * 50U) / 100U;
    } else {
        analyzer->ceiling_metric = (analyzer->ceiling_metric * 98U + metric * 2U) / 100U;
    }

    uint32_t min_span = (uint32_t)CONFIG_ORB_AUDIO_REACTIVE_MIN_SPAN;
    if ((analyzer->ceiling_metric - analyzer->floor_metric) < min_span) {
        analyzer->ceiling_metric = analyzer->floor_metric + min_span;
    }

    uint32_t span = analyzer->ceiling_metric - analyzer->floor_metric;
    uint32_t norm = 0U;
    if (metric > analyzer->floor_metric && span > 0U) {
        norm = ((metric - analyzer->floor_metric) * 255U) / span;
        if (norm > 255U) {
            norm = 255U;
        }
    }

    if (norm <= (uint32_t)CONFIG_ORB_AUDIO_REACTIVE_NOISE_GATE) {
        norm = 0U;
    }

    uint8_t raw = (uint8_t)norm;
    if (raw >= analyzer->filtered_level) {
        analyzer->filtered_level =
            (uint8_t)(((uint16_t)analyzer->filtered_level * 35U + (uint16_t)raw * 65U + 50U) / 100U);
    } else {
        analyzer->filtered_level =
            (uint8_t)(((uint16_t)analyzer->filtered_level * 58U + (uint16_t)raw * 42U + 50U) / 100U);
    }
}

uint8_t audio_reactive_analyzer_get_level(const audio_reactive_analyzer_t *analyzer)
{
    if (analyzer == NULL) {
        return 0U;
    }
    return analyzer->filtered_level;
}

