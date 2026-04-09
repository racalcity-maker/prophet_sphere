#include "audio_worker_resampler.h"

#include <inttypes.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "audio_worker_mixer.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;

static uint32_t s_fg_src_rate_hz;
static uint32_t s_rs_step_q16;
static uint32_t s_rs_phase_q16;
static int16_t s_rs_prev_sample;
static bool s_rs_prev_valid;
static int16_t s_rs_out_buffer[AUDIO_MIX_BUFFER_SAMPLES];

void audio_worker_resampler_reset(void)
{
    s_fg_src_rate_hz = 0U;
    s_rs_step_q16 = 0U;
    s_rs_phase_q16 = 0U;
    s_rs_prev_sample = 0;
    s_rs_prev_valid = false;
}

static void audio_worker_resampler_set_source_rate(uint32_t src_rate_hz)
{
    if (src_rate_hz == 0U) {
        src_rate_hz = (uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ;
    }
    if (s_fg_src_rate_hz == src_rate_hz && s_rs_step_q16 != 0U) {
        return;
    }

    s_fg_src_rate_hz = src_rate_hz;
    uint64_t numer = ((uint64_t)src_rate_hz << 16);
    uint32_t denom = (uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ;
    uint32_t step = (denom > 0U) ? (uint32_t)(numer / (uint64_t)denom) : 0U;
    if (step == 0U) {
        step = 1U;
    }
    s_rs_step_q16 = step;
    s_rs_phase_q16 = 0U;
    s_rs_prev_valid = false;

    if (src_rate_hz != (uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ) {
        ESP_LOGI(TAG,
                 "MP3 sample-rate convert: src=%" PRIu32 " -> out=%u",
                 src_rate_hz,
                 (unsigned)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ);
    }
}

esp_err_t audio_worker_resampler_write_fg(audio_worker_shared_state_t *state,
                                          const int16_t *samples,
                                          size_t sample_count,
                                          uint32_t src_rate_hz)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (samples == NULL || sample_count == 0U) {
        return ESP_OK;
    }

    if (src_rate_hz == 0U || src_rate_hz == (uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ) {
        return audio_worker_write_mixed_output(state, samples, sample_count, true);
    }

    audio_worker_resampler_set_source_rate(src_rate_hz);

    size_t in_idx = 0U;
    if (!s_rs_prev_valid) {
        s_rs_prev_sample = samples[0];
        s_rs_prev_valid = true;
        in_idx = 1U;
        if (sample_count < 2U) {
            return ESP_OK;
        }
    }

    size_t out_count = 0U;
    for (; in_idx < sample_count; ++in_idx) {
        int16_t s0 = s_rs_prev_sample;
        int16_t s1 = samples[in_idx];
        int32_t delta = (int32_t)s1 - (int32_t)s0;

        while (s_rs_phase_q16 < 65536U) {
            int32_t mix = (int32_t)s0 + (int32_t)(((int64_t)delta * (int64_t)s_rs_phase_q16) >> 16);
            s_rs_out_buffer[out_count++] = (int16_t)mix;
            if (out_count >= AUDIO_MIX_BUFFER_SAMPLES) {
                esp_err_t wr_err = audio_worker_write_mixed_output(state, s_rs_out_buffer, out_count, true);
                if (wr_err != ESP_OK) {
                    return wr_err;
                }
                out_count = 0U;
            }
            s_rs_phase_q16 += s_rs_step_q16;
        }

        s_rs_phase_q16 -= 65536U;
        s_rs_prev_sample = s1;
    }

    if (out_count > 0U) {
        return audio_worker_write_mixed_output(state, s_rs_out_buffer, out_count, true);
    }
    return ESP_OK;
}
