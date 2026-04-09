#include "rest_api_talk_live_buffer.h"

#include <stddef.h>

void talk_live_buffer_reset(talk_live_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }
    buffer->write_pos = 0U;
    buffer->read_pos = 0U;
    buffer->count = 0U;
}

uint32_t talk_live_buffer_push(talk_live_buffer_t *buffer, const int16_t *samples, uint32_t sample_count)
{
    if (buffer == NULL || samples == NULL || sample_count == 0U ||
        buffer->samples == NULL || buffer->capacity_samples == 0U) {
        return 0U;
    }

    uint32_t accepted = 0U;
    while (accepted < sample_count) {
        if (buffer->count >= buffer->capacity_samples) {
            buffer->dropped_samples += (sample_count - accepted);
            break;
        }
        buffer->samples[buffer->write_pos] = samples[accepted];
        buffer->write_pos++;
        if (buffer->write_pos >= buffer->capacity_samples) {
            buffer->write_pos = 0U;
        }
        buffer->count++;
        accepted++;
    }
    return accepted;
}

uint16_t talk_live_buffer_pop(talk_live_buffer_t *buffer, int16_t *out_samples, uint16_t max_samples)
{
    if (buffer == NULL || out_samples == NULL || max_samples == 0U ||
        buffer->samples == NULL || buffer->capacity_samples == 0U) {
        return 0U;
    }

    uint32_t to_copy = buffer->count;
    if (to_copy > max_samples) {
        to_copy = max_samples;
    }
    uint16_t copied = 0U;
    while (copied < to_copy) {
        out_samples[copied++] = buffer->samples[buffer->read_pos];
        buffer->read_pos++;
        if (buffer->read_pos >= buffer->capacity_samples) {
            buffer->read_pos = 0U;
        }
        buffer->count--;
    }
    return copied;
}

