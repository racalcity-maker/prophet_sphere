#include "mic_task_loopback.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "app_tasking.h"
#include "esp_log.h"
#include "mic_task_capture_ws.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORB_MIC_SAMPLE_RATE_HZ
#define CONFIG_ORB_MIC_SAMPLE_RATE_HZ 16000
#endif
#ifndef CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ
#define CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ 44100
#endif

#define MIC_LOOPBACK_GAIN_NUM 3
#define MIC_LOOPBACK_GAIN_DEN 1

/* Single mic_task producer; static storage avoids large per-call stack frames. */
static int16_t s_loopback_out_buf[AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES];
static audio_command_t s_loopback_pcm_cmd;

static bool push_pcm_stream_chunk(const int16_t *samples, uint16_t sample_count, uint32_t timeout_ms)
{
    if (samples == NULL || sample_count == 0U || sample_count > AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES) {
        return false;
    }

    memset(&s_loopback_pcm_cmd, 0, sizeof(s_loopback_pcm_cmd));
    s_loopback_pcm_cmd.id = AUDIO_CMD_PCM_STREAM_CHUNK;
    s_loopback_pcm_cmd.payload.pcm_stream_chunk.sample_count = sample_count;
    memcpy(s_loopback_pcm_cmd.payload.pcm_stream_chunk.samples, samples, (size_t)sample_count * sizeof(int16_t));
    return (app_tasking_send_audio_command(&s_loopback_pcm_cmd, timeout_ms) == ESP_OK);
}

static int16_t apply_loopback_gain(int16_t sample)
{
    int32_t v = (int32_t)sample;
    v = (v * MIC_LOOPBACK_GAIN_NUM) / MIC_LOOPBACK_GAIN_DEN;
    if (v > INT16_MAX) {
        v = INT16_MAX;
    } else if (v < INT16_MIN) {
        v = INT16_MIN;
    }
    return (int16_t)v;
}

void mic_task_loopback_stream_samples(uint32_t *phase_accum, const int32_t *samples, size_t sample_count, const char *log_tag)
{
    if (phase_accum == NULL || samples == NULL || sample_count == 0U) {
        return;
    }

    uint16_t out_used = 0U;
    uint32_t dropped_chunks = 0U;

    for (size_t i = 0; i < sample_count; ++i) {
        int16_t pcm = mic_task_capture_raw_to_pcm16(samples[i]);
        *phase_accum += (uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ;
        while (*phase_accum >= (uint32_t)CONFIG_ORB_MIC_SAMPLE_RATE_HZ) {
            *phase_accum -= (uint32_t)CONFIG_ORB_MIC_SAMPLE_RATE_HZ;
            s_loopback_out_buf[out_used++] = apply_loopback_gain(pcm);
            if (out_used == AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES) {
                if (!push_pcm_stream_chunk(s_loopback_out_buf, out_used, 0U)) {
                    dropped_chunks++;
                }
                out_used = 0U;
            }
        }
    }

    if (out_used > 0U) {
        if (!push_pcm_stream_chunk(s_loopback_out_buf, out_used, 0U)) {
            dropped_chunks++;
        }
    }

    if (dropped_chunks > 0U) {
        ESP_LOGW(log_tag, "loopback chunk drops=%" PRIu32, dropped_chunks);
    }
}
