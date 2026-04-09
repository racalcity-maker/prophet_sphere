#ifndef REST_API_TALK_LIVE_BUFFER_H
#define REST_API_TALK_LIVE_BUFFER_H

#include <stdint.h>

typedef struct {
    int16_t *samples;
    uint32_t capacity_samples;
    uint32_t write_pos;
    uint32_t read_pos;
    uint32_t count;
    uint32_t dropped_samples;
} talk_live_buffer_t;

void talk_live_buffer_reset(talk_live_buffer_t *buffer);
uint32_t talk_live_buffer_push(talk_live_buffer_t *buffer, const int16_t *samples, uint32_t sample_count);
uint16_t talk_live_buffer_pop(talk_live_buffer_t *buffer, int16_t *out_samples, uint16_t max_samples);

#endif

