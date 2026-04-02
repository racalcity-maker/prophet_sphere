#ifndef AUDIO_REACTIVE_ANALYZER_H
#define AUDIO_REACTIVE_ANALYZER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t floor_metric;
    uint32_t ceiling_metric;
    uint8_t filtered_level;
    int16_t prev_sample;
    uint8_t prev_valid;
} audio_reactive_analyzer_t;

void audio_reactive_analyzer_init(audio_reactive_analyzer_t *analyzer);
void audio_reactive_analyzer_reset(audio_reactive_analyzer_t *analyzer);
void audio_reactive_analyzer_process_pcm16_mono(audio_reactive_analyzer_t *analyzer,
                                                const int16_t *samples,
                                                size_t sample_count);
uint8_t audio_reactive_analyzer_get_level(const audio_reactive_analyzer_t *analyzer);

#endif

