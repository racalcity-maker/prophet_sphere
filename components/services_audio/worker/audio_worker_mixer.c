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

/* Local state shorthands (kept file-local on purpose). */
#define s_pcm_stream_active (state->pcm_stream_active)
#define s_volume (state->volume)
#define s_bg (state->bg)
#define s_fg_attack_active (state->fg_attack_active)
#define s_fg_attack_total_samples (state->fg_attack_total_samples)
#define s_fg_attack_done_samples (state->fg_attack_done_samples)
#define s_fg_content_started (state->fg_content_started)
#define s_mix_buffer (state->mix_buffer)
#define s_bg_buffer (state->bg_buffer)
static TickType_t s_tts_mix_diag_last_log_tick;
static bool s_tts_out_prev_sample_valid;
static int16_t s_tts_out_prev_sample;
static uint32_t s_tts_out_boundary_jump_count;
static uint32_t s_tts_out_boundary_jump_max;
static bool s_fg_prev_sample_valid;
static int16_t s_fg_prev_sample;
static uint32_t s_fg_boundary_jump_count;
static uint32_t s_fg_boundary_jump_max;
static bool s_pcm_fg_signal_prev;
static uint16_t s_pcm_fg_silence_run;
static uint32_t s_click_probe_budget;
static uint64_t s_click_probe_emitted_samples;
static bool s_mix_warned_null_fg_ptr;
static bool s_mix_warned_oversized_chunk;

#define AUDIO_OUT_DECLICK_THRESHOLD 2200U
#define AUDIO_OUT_DECLICK_RAMP_SAMPLES 48U
#define AUDIO_FG_DECLICK_THRESHOLD 900U
#define AUDIO_FG_DECLICK_RAMP_SAMPLES 96U
#define AUDIO_FG_ONSET_ON_THRESHOLD 600U
#define AUDIO_FG_ONSET_OFF_THRESHOLD 240U
#define AUDIO_FG_ONSET_SILENCE_SAMPLES 2205U
#define AUDIO_CLICK_PROBE_BUDGET 0U
#define AUDIO_CLICK_SPIKE_ABS 2200
#define AUDIO_CLICK_NEIGHBOR_ABS 520
#define AUDIO_CLICK_DELTA 2000

typedef struct {
    bool *prev_valid;
    int16_t *prev_sample;
    uint32_t *jump_count;
    uint32_t *jump_max;
    uint32_t threshold;
    size_t ramp_samples;
} boundary_declick_cfg_t;

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

static uint16_t fg_gain_permille(audio_worker_shared_state_t *state)
{
    if (state == NULL) {
        return 0U;
    }
    uint32_t gain = ((uint32_t)s_volume * 1000U) / 100U;
    if (gain > 1000U) {
        gain = 1000U;
    }
    return (uint16_t)gain;
}

void audio_worker_fg_attack_reset(audio_worker_shared_state_t *state)
{
    if (state == NULL) {
        return;
    }
    s_fg_attack_total_samples = audio_worker_ms_to_samples((uint32_t)CONFIG_ORB_AUDIO_FG_ATTACK_MS);
    s_fg_attack_done_samples = 0U;
    s_fg_attack_active = (s_fg_attack_total_samples > 0U);
}

uint16_t audio_worker_fg_attack_next_permille(audio_worker_shared_state_t *state)
{
    if (state == NULL) {
        return 1000U;
    }
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

void audio_worker_pcm_stream_diag_reset(audio_worker_shared_state_t *state)
{
    (void)state;
    s_tts_mix_diag_last_log_tick = 0;
    s_tts_out_prev_sample_valid = true;
    s_tts_out_prev_sample = 0;
    s_tts_out_boundary_jump_count = 0U;
    s_tts_out_boundary_jump_max = 0U;
    s_fg_prev_sample_valid = true;
    s_fg_prev_sample = 0;
    s_fg_boundary_jump_count = 0U;
    s_fg_boundary_jump_max = 0U;
    s_pcm_fg_signal_prev = false;
    s_pcm_fg_silence_run = AUDIO_FG_ONSET_SILENCE_SAMPLES;
    s_click_probe_budget = AUDIO_CLICK_PROBE_BUDGET;
    s_click_probe_emitted_samples = 0U;
    s_mix_warned_null_fg_ptr = false;
    s_mix_warned_oversized_chunk = false;
}

void audio_worker_pcm_stream_diag_snapshot(audio_worker_shared_state_t *state, uint32_t *out_jump_count, uint32_t *out_jump_max)
{
    (void)state;
    if (out_jump_count != NULL) {
        *out_jump_count = s_tts_out_boundary_jump_count;
    }
    if (out_jump_max != NULL) {
        *out_jump_max = s_tts_out_boundary_jump_max;
    }
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

static uint16_t bg_gain_for_sample_offset(audio_worker_shared_state_t *state, size_t sample_offset)
{
    if (state == NULL) {
        return 0U;
    }
    if (!s_bg.fade_active || s_bg.fade_total_samples == 0U) {
        return s_bg.gain_permille;
    }

    uint64_t pos = (uint64_t)s_bg.fade_done_samples + (uint64_t)sample_offset;
    if (pos >= (uint64_t)s_bg.fade_total_samples) {
        return s_bg.fade_target_gain_permille;
    }

    int32_t delta = (int32_t)s_bg.fade_target_gain_permille - (int32_t)s_bg.fade_start_gain_permille;
    int64_t numer = (int64_t)delta * (int64_t)pos;
    int32_t step = (int32_t)(numer / (int64_t)s_bg.fade_total_samples);
    int32_t gain = (int32_t)s_bg.fade_start_gain_permille + step;
    if (gain < 0) {
        gain = 0;
    } else if (gain > 1000) {
        gain = 1000;
    }
    return (uint16_t)gain;
}

static void apply_boundary_declick(int16_t *samples, size_t sample_count, const boundary_declick_cfg_t *cfg)
{
    if (samples == NULL || sample_count == 0U || cfg == NULL || cfg->prev_valid == NULL ||
        cfg->prev_sample == NULL || cfg->jump_count == NULL || cfg->jump_max == NULL) {
        return;
    }

    int16_t first = samples[0];
    int16_t last = samples[sample_count - 1U];
    if (!(*cfg->prev_valid)) {
        *cfg->prev_sample = last;
        *cfg->prev_valid = true;
        return;
    }

    int32_t jump = (int32_t)first - (int32_t)(*cfg->prev_sample);
    if (jump < 0) {
        jump = -jump;
    }
    uint32_t abs_jump = (uint32_t)jump;
    if (abs_jump > *cfg->jump_max) {
        *cfg->jump_max = abs_jump;
    }
    if (abs_jump >= cfg->threshold) {
        size_t ramp_n = sample_count;
        if (ramp_n > cfg->ramp_samples) {
            ramp_n = cfg->ramp_samples;
        }
        int32_t prev = (int32_t)(*cfg->prev_sample);
        for (size_t i = 0; i < ramp_n; ++i) {
            int32_t target = (int32_t)samples[i];
            int32_t mixed = prev + (int32_t)(((int64_t)(target - prev) * (int64_t)(i + 1U)) / (int64_t)ramp_n);
            samples[i] = clamp_i16(mixed);
        }
        (*cfg->jump_count)++;
        last = samples[sample_count - 1U];
    }

    *cfg->prev_sample = last;
    *cfg->prev_valid = true;
}

static void audio_worker_apply_output_declick(int16_t *samples, size_t sample_count)
{
    const boundary_declick_cfg_t cfg = {
        .prev_valid = &s_tts_out_prev_sample_valid,
        .prev_sample = &s_tts_out_prev_sample,
        .jump_count = &s_tts_out_boundary_jump_count,
        .jump_max = &s_tts_out_boundary_jump_max,
        .threshold = AUDIO_OUT_DECLICK_THRESHOLD,
        .ramp_samples = AUDIO_OUT_DECLICK_RAMP_SAMPLES,
    };
    apply_boundary_declick(samples, sample_count, &cfg);
}

static void audio_worker_apply_fg_declick(int16_t *samples, size_t sample_count)
{
    const boundary_declick_cfg_t cfg = {
        .prev_valid = &s_fg_prev_sample_valid,
        .prev_sample = &s_fg_prev_sample,
        .jump_count = &s_fg_boundary_jump_count,
        .jump_max = &s_fg_boundary_jump_max,
        .threshold = AUDIO_FG_DECLICK_THRESHOLD,
        .ramp_samples = AUDIO_FG_DECLICK_RAMP_SAMPLES,
    };
    apply_boundary_declick(samples, sample_count, &cfg);
}

static void audio_worker_probe_click_spike(const int16_t *samples, size_t sample_count, const char *stage)
{
    if (samples == NULL || sample_count < 3U || s_click_probe_budget == 0U) {
        return;
    }
    for (size_t i = 1U; i + 1U < sample_count; ++i) {
        int32_t p = (int32_t)samples[i - 1U];
        int32_t c = (int32_t)samples[i];
        int32_t n = (int32_t)samples[i + 1U];
        int32_t ap = (p < 0) ? -p : p;
        int32_t ac = (c < 0) ? -c : c;
        int32_t an = (n < 0) ? -n : n;
        int32_t d1 = c - p;
        int32_t d2 = c - n;
        if (d1 < 0) {
            d1 = -d1;
        }
        if (d2 < 0) {
            d2 = -d2;
        }
        if (ac >= AUDIO_CLICK_SPIKE_ABS &&
            ap <= AUDIO_CLICK_NEIGHBOR_ABS &&
            an <= AUDIO_CLICK_NEIGHBOR_ABS &&
            d1 >= AUDIO_CLICK_DELTA &&
            d2 >= AUDIO_CLICK_DELTA) {
            uint64_t at_sample = s_click_probe_emitted_samples + (uint64_t)i;
            uint32_t at_ms = (uint32_t)((at_sample * 1000ULL) / (uint64_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ);
            ESP_LOGW(TAG,
                     "click_probe stage=%s at=%" PRIu64 " (%" PRIu32 "ms) p=%ld c=%ld n=%ld d1=%ld d2=%ld",
                     stage,
                     at_sample,
                     at_ms,
                     (long)p,
                     (long)c,
                     (long)n,
                     (long)d1,
                     (long)d2);
            s_click_probe_budget--;
            break;
        }
    }
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

static void audio_worker_reset_fg_chain_state(void)
{
    s_pcm_fg_signal_prev = false;
    s_pcm_fg_silence_run = AUDIO_FG_ONSET_SILENCE_SAMPLES;
    s_fg_prev_sample = 0;
    s_fg_prev_sample_valid = true;
}

static void audio_worker_mix_background_into_output(audio_worker_shared_state_t *state, size_t sample_count)
{
    if (state == NULL) {
        return;
    }
    if (!s_bg.active || sample_count == 0U) {
        return;
    }

    size_t got = audio_worker_bg_read_samples(state, s_bg_buffer, sample_count);
    if (got < sample_count) {
        (void)memset(&s_bg_buffer[got], 0, (sample_count - got) * sizeof(int16_t));
    }

    if (s_bg.fade_active) {
        for (size_t i = 0; i < sample_count; ++i) {
            uint16_t bg_gain = bg_gain_for_sample_offset(state, i);
            int32_t mixed = (int32_t)s_mix_buffer[i];
            mixed += ((int32_t)s_bg_buffer[i] * (int32_t)bg_gain) / 1000;
            s_mix_buffer[i] = clamp_i16(mixed);
        }
    } else {
        uint16_t bg_gain = s_bg.gain_permille;
        for (size_t i = 0; i < sample_count; ++i) {
            int32_t mixed = (int32_t)s_mix_buffer[i];
            mixed += ((int32_t)s_bg_buffer[i] * (int32_t)bg_gain) / 1000;
            s_mix_buffer[i] = clamp_i16(mixed);
        }
    }

    /*
     * Fade state ownership remains in audio_worker_bg_update_fade();
     * here we only read interpolated gain for smoother per-sample mix.
     */
    audio_worker_bg_update_fade(state, sample_count);
}

static void audio_worker_log_tts_mix_diag(audio_worker_shared_state_t *state, const int16_t *fg_chunk, size_t composed)
{
    if (state == NULL) {
        return;
    }
    if (fg_chunk == NULL || composed == 0U || !s_pcm_stream_active) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    TickType_t gap = audio_worker_ms_to_ticks_min1(1000U);
    if ((now - s_tts_mix_diag_last_log_tick) < gap) {
        return;
    }
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
             " bg=%u bg_gain=%u vol=%u fg_jumps=%" PRIu32 " fg_jump_max=%" PRIu32
             " out_jumps=%" PRIu32 " out_jump_max=%" PRIu32,
             (int)fg_min,
             (int)fg_max,
             fg_abs_avg,
             (int)out_min,
             (int)out_max,
             out_abs_avg,
             (unsigned)out_clip_pm,
             s_bg.active ? 1U : 0U,
             (unsigned)s_bg.gain_permille,
             (unsigned)s_volume,
             s_fg_boundary_jump_count,
             s_fg_boundary_jump_max,
             s_tts_out_boundary_jump_count,
             s_tts_out_boundary_jump_max);
}

size_t audio_worker_compose_mixed_chunk(audio_worker_shared_state_t *state,
                                        const int16_t *fg_samples,
                                        size_t sample_count,
                                        bool has_foreground)
{
    if (state == NULL) {
        return 0U;
    }
    if (sample_count > AUDIO_MIX_BUFFER_SAMPLES) {
        if (!s_mix_warned_oversized_chunk) {
            s_mix_warned_oversized_chunk = true;
            ESP_LOGW(TAG,
                     "mixer contract: chunk %u > mix buffer %u, clamping",
                     (unsigned)sample_count,
                     (unsigned)AUDIO_MIX_BUFFER_SAMPLES);
        }
        sample_count = AUDIO_MIX_BUFFER_SAMPLES;
    }
    if (sample_count == 0U) {
        return 0U;
    }
    bool fg_present = has_foreground && (fg_samples != NULL);
    if (has_foreground && fg_samples == NULL && !s_mix_warned_null_fg_ptr) {
        s_mix_warned_null_fg_ptr = true;
        ESP_LOGW(TAG, "mixer contract: has_foreground=1 but fg_samples is NULL");
    }
    /* Keep DAC fed with explicit silence while PCM stream is active but
     * foreground chunks are temporarily absent (tail/drain window). Without
     * this, some I2S/DMA paths may hold/repeat last non-zero fragment. */
    if (!fg_present && !s_bg.active && !s_pcm_stream_active) {
        return 0U;
    }

    if (fg_present) {
        if (s_pcm_stream_active) {
            uint16_t base_gain = fg_gain_permille(state);
            for (size_t i = 0; i < sample_count; ++i) {
                int16_t in = fg_samples[i];
                int32_t iv = (int32_t)in;
                int32_t abs_iv = (iv < 0) ? -iv : iv;
                bool voiced_now = s_pcm_fg_signal_prev;
                bool onset = false;

                if (abs_iv <= (int32_t)AUDIO_FG_ONSET_OFF_THRESHOLD) {
                    if (s_pcm_fg_silence_run < 0xFFFFU) {
                        s_pcm_fg_silence_run++;
                    }
                    if (s_pcm_fg_silence_run >= AUDIO_FG_ONSET_SILENCE_SAMPLES) {
                        voiced_now = false;
                    }
                } else if (abs_iv >= (int32_t)AUDIO_FG_ONSET_ON_THRESHOLD) {
                    if (!s_pcm_fg_signal_prev && s_pcm_fg_silence_run >= AUDIO_FG_ONSET_SILENCE_SAMPLES) {
                        /* Re-arm attack on phrase start after real silence window. */
                        audio_worker_fg_attack_reset(state);
                        onset = true;
                    }
                    s_pcm_fg_silence_run = 0U;
                    voiced_now = true;
                } else {
                    /* Hysteresis middle band: keep previous state. */
                    if (s_pcm_fg_signal_prev) {
                        s_pcm_fg_silence_run = 0U;
                    }
                }

                s_pcm_fg_signal_prev = voiced_now;
                (void)onset;

                uint16_t attack_gain = audio_worker_fg_attack_next_permille(state);
                int64_t scaled = (int64_t)in;
                scaled = (scaled * (int64_t)base_gain) / 1000LL;
                scaled = (scaled * (int64_t)attack_gain) / 1000LL;
                s_mix_buffer[i] = clamp_i16((int32_t)scaled);
            }
        } else {
            if (!s_fg_content_started) {
                if (audio_worker_pcm_has_signal(fg_samples, sample_count, CONFIG_ORB_AUDIO_FG_SIGNAL_ABS_THRESHOLD)) {
                    s_fg_content_started = true;
                    audio_worker_fg_attack_reset(state);
                } else {
                    (void)memset(s_mix_buffer, 0, sample_count * sizeof(int16_t));
                }
            }

            if (s_fg_content_started) {
                uint16_t base_gain = fg_gain_permille(state);
                for (size_t i = 0; i < sample_count; ++i) {
                    uint16_t attack_gain = audio_worker_fg_attack_next_permille(state);
                    int64_t scaled = (int64_t)fg_samples[i];
                    scaled = (scaled * (int64_t)base_gain) / 1000LL;
                    scaled = (scaled * (int64_t)attack_gain) / 1000LL;
                    s_mix_buffer[i] = clamp_i16((int32_t)scaled);
                }
            }
        }
        audio_worker_apply_fg_declick(s_mix_buffer, sample_count);
    } else {
        (void)memset(s_mix_buffer, 0, sample_count * sizeof(int16_t));
        audio_worker_reset_fg_chain_state();
    }
    audio_worker_mix_background_into_output(state, sample_count);

    return sample_count;
}

esp_err_t audio_worker_write_mixed_output(audio_worker_shared_state_t *state,
                                          const int16_t *fg_samples,
                                          size_t sample_count,
                                          bool has_foreground)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (has_foreground && fg_samples == NULL) {
        ESP_LOGW(TAG, "mixer write contract: foreground chunk requested with NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }
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
        size_t composed = audio_worker_compose_mixed_chunk(state, fg_chunk, chunk, has_foreground);
        if (composed == 0U) {
            return ESP_OK;
        }

        audio_worker_apply_output_declick(s_mix_buffer, composed);
        audio_worker_probe_click_spike(s_mix_buffer, composed, "out");

        if (has_foreground && fg_chunk != NULL) {
            audio_worker_audio_level_process_samples(state, fg_chunk, composed);
            audio_worker_log_tts_mix_diag(state, fg_chunk, composed);
        }
        esp_err_t wr_err =
            audio_output_i2s_write_mono_pcm16(s_mix_buffer, composed, (uint32_t)CONFIG_ORB_AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (wr_err != ESP_OK) {
            return wr_err;
        }
        s_click_probe_emitted_samples += (uint64_t)composed;

        if (!has_foreground && sample_count == 0U) {
            return ESP_OK;
        }
        offset += composed;
    }
    return ESP_OK;
}
