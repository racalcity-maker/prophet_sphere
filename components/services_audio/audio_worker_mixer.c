#include "audio_worker_mixer.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "audio_output_i2s.h"
#include "audio_worker_bg.h"
#include "audio_worker_internal.h"
#include "audio_worker_reactive.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;
static TickType_t s_tts_mix_diag_last_log_tick;

static inline int16_t clamp_i16(int32_t value)
{
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return (int16_t)value;
}

static uint16_t fg_gain_permille(void)
{
    uint32_t gain = ((uint32_t)s_volume * 1000U) / 100U;
    if (gain > 1000U) {
        gain = 1000U;
    }
    return (uint16_t)gain;
}

void audio_worker_fg_attack_reset(void)
{
    s_fg_attack_total_samples = audio_worker_ms_to_samples((uint32_t)CONFIG_ORB_AUDIO_FG_ATTACK_MS);
    s_fg_attack_done_samples = 0U;
    s_fg_attack_active = (s_fg_attack_total_samples > 0U);
}

uint16_t audio_worker_fg_attack_next_permille(void)
{
    if (!s_fg_attack_active) {
        return 1000U;
    }
    if (s_fg_attack_total_samples == 0U) {
        s_fg_attack_active = false;
        return 1000U;
    }

    uint32_t done = s_fg_attack_done_samples;
    if (done >= s_fg_attack_total_samples) {
        s_fg_attack_active = false;
        return 1000U;
    }

    uint32_t gain = (done * 1000U) / s_fg_attack_total_samples;
    s_fg_attack_done_samples = done + 1U;
    if (s_fg_attack_done_samples >= s_fg_attack_total_samples) {
        s_fg_attack_active = false;
    }
    if (gain > 1000U) {
        gain = 1000U;
    }
    return (uint16_t)gain;
}

static void pcm_short_stats(const int16_t *samples, size_t sample_count, int16_t *out_min, int16_t *out_max, uint32_t *out_abs_avg)
{
    if (samples == NULL || sample_count == 0U || out_min == NULL || out_max == NULL || out_abs_avg == NULL) {
        return;
    }
    size_t n = sample_count;
    if (n > 512U) {
        n = 512U;
    }
    int16_t min_v = 32767;
    int16_t max_v = -32768;
    uint64_t sum_abs = 0U;
    for (size_t i = 0; i < n; ++i) {
        int16_t v = samples[i];
        if (v < min_v) {
            min_v = v;
        }
        if (v > max_v) {
            max_v = v;
        }
        int32_t iv = (int32_t)v;
        sum_abs += (uint64_t)((iv < 0) ? -iv : iv);
    }
    *out_min = min_v;
    *out_max = max_v;
    *out_abs_avg = (uint32_t)(sum_abs / (uint64_t)n);
}

static uint16_t pcm_clip_permille(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0U) {
        return 0U;
    }
    size_t n = sample_count;
    if (n > 1024U) {
        n = 1024U;
    }
    uint32_t clips = 0U;
    for (size_t i = 0; i < n; ++i) {
        int16_t v = samples[i];
        if (v >= 32760 || v <= -32760) {
            clips++;
        }
    }
    return (uint16_t)((clips * 1000U) / n);
}

bool audio_worker_pcm_has_signal(const int16_t *samples, size_t sample_count, uint32_t abs_avg_threshold)
{
    if (samples == NULL || sample_count == 0U) {
        return false;
    }
    int16_t min_v = 0;
    int16_t max_v = 0;
    uint32_t avg_abs = 0U;
    pcm_short_stats(samples, sample_count, &min_v, &max_v, &avg_abs);
    (void)min_v;
    (void)max_v;
    return avg_abs >= abs_avg_threshold;
}

size_t audio_worker_compose_mixed_chunk(const int16_t *fg_samples, size_t sample_count, bool has_foreground)
{
    if (sample_count == 0U) {
        return 0U;
    }
    if (!has_foreground && !s_bg.active) {
        return 0U;
    }

    if (has_foreground && fg_samples != NULL) {
        if (s_pcm_stream_active) {
            uint16_t base_gain = fg_gain_permille();
            for (size_t i = 0; i < sample_count; ++i) {
                int64_t scaled = (int64_t)fg_samples[i];
                scaled = (scaled * (int64_t)base_gain) / 1000LL;
                s_mix_buffer[i] = clamp_i16((int32_t)scaled);
            }
        } else {
            if (!s_fg_content_started) {
                if (audio_worker_pcm_has_signal(fg_samples, sample_count, CONFIG_ORB_AUDIO_FG_SIGNAL_ABS_THRESHOLD)) {
                    s_fg_content_started = true;
                    audio_worker_fg_attack_reset();
                } else {
                    (void)memset(s_mix_buffer, 0, sample_count * sizeof(int16_t));
                }
            }

            if (s_fg_content_started) {
                uint16_t base_gain = fg_gain_permille();
                for (size_t i = 0; i < sample_count; ++i) {
                    uint16_t attack_gain = audio_worker_fg_attack_next_permille();
                    int64_t scaled = (int64_t)fg_samples[i];
                    scaled = (scaled * (int64_t)base_gain) / 1000LL;
                    scaled = (scaled * (int64_t)attack_gain) / 1000LL;
                    s_mix_buffer[i] = clamp_i16((int32_t)scaled);
                }
            }
        }
    } else {
        (void)memset(s_mix_buffer, 0, sample_count * sizeof(int16_t));
    }

    if (s_bg.active) {
        size_t got = audio_worker_bg_read_samples(s_bg_buffer, sample_count);
        if (got < sample_count) {
            (void)memset(&s_bg_buffer[got], 0, (sample_count - got) * sizeof(int16_t));
        }

        uint16_t bg_gain = s_bg.gain_permille;
        for (size_t i = 0; i < sample_count; ++i) {
            int32_t mixed = (int32_t)s_mix_buffer[i];
            mixed += ((int32_t)s_bg_buffer[i] * (int32_t)bg_gain) / 1000;
            s_mix_buffer[i] = clamp_i16(mixed);
        }
        audio_worker_bg_update_fade(sample_count);
    }

    return sample_count;
}

esp_err_t audio_worker_write_mixed_output(const int16_t *fg_samples, size_t sample_count, bool has_foreground)
{
    size_t offset = 0U;
    while (offset < sample_count || (!has_foreground && sample_count == 0U)) {
        size_t chunk = sample_count - offset;
        if (chunk > AUDIO_MIX_BUFFER_SAMPLES) {
            chunk = AUDIO_MIX_BUFFER_SAMPLES;
        }
        if (!has_foreground && sample_count == 0U) {
            chunk = AUDIO_BG_ONLY_CHUNK_SAMPLES;
        }

        const int16_t *fg_chunk = has_foreground ? &fg_samples[offset] : NULL;
        size_t composed = audio_worker_compose_mixed_chunk(fg_chunk, chunk, has_foreground);
        if (composed == 0U) {
            return ESP_OK;
        }

        if (has_foreground && fg_chunk != NULL) {
            audio_worker_audio_level_process_samples(fg_chunk, composed);
            if (s_pcm_stream_active) {
                TickType_t now = xTaskGetTickCount();
                TickType_t gap = audio_worker_ms_to_ticks_min1(1000U);
                if ((now - s_tts_mix_diag_last_log_tick) >= gap) {
                    s_tts_mix_diag_last_log_tick = now;
                    int16_t fg_min = 0;
                    int16_t fg_max = 0;
                    uint32_t fg_abs_avg = 0U;
                    int16_t out_min = 0;
                    int16_t out_max = 0;
                    uint32_t out_abs_avg = 0U;
                    uint16_t out_clip_pm = 0U;
                    pcm_short_stats(fg_chunk, composed, &fg_min, &fg_max, &fg_abs_avg);
                    pcm_short_stats(s_mix_buffer, composed, &out_min, &out_max, &out_abs_avg);
                    out_clip_pm = pcm_clip_permille(s_mix_buffer, composed);
                    ESP_LOGD(TAG,
                             "tts mix diag fg[min=%d max=%d abs_avg=%" PRIu32 "]"
                             " out[min=%d max=%d abs_avg=%" PRIu32 " clip=%u/1000]"
                             " bg=%u bg_gain=%u vol=%u",
                             (int)fg_min,
                             (int)fg_max,
                             fg_abs_avg,
                             (int)out_min,
                             (int)out_max,
                             out_abs_avg,
                             (unsigned)out_clip_pm,
                             s_bg.active ? 1U : 0U,
                             (unsigned)s_bg.gain_permille,
                             (unsigned)s_volume);
                }
            }
        }
        esp_err_t wr_err =
            audio_output_i2s_write_mono_pcm16(s_mix_buffer, composed, (uint32_t)CONFIG_ORB_AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (wr_err != ESP_OK) {
            return wr_err;
        }

        if (!has_foreground && sample_count == 0U) {
            return ESP_OK;
        }
        offset += composed;
    }
    return ESP_OK;
}
