#include "audio_worker_sim.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "audio_worker_internal.h"
#include "audio_worker_mixer.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;

/* Fallback melody: do-re-mi-do-re-do, repeated 2 times; 180 ms per note + 40 ms gap. */
#define AUDIO_FALLBACK_MELODY_NOTE_MS 180U
#define AUDIO_FALLBACK_MELODY_GAP_MS 40U
static const uint16_t s_fallback_melody_hz[] = {
    523U, 587U, 659U, 523U, 587U, 523U,
    523U, 587U, 659U, 523U, 587U, 523U
};

#if CONFIG_ORB_AUDIO_TEST_TONE_ENABLE
static uint32_t s_tone_phase_q32;
static uint32_t s_tone_inc_q32;
static TickType_t s_last_tone_tick;
static TickType_t s_last_tone_err_tick;
static int16_t s_tone_buffer[256];
static uint8_t s_sim_note_index;
static bool s_sim_tone_muted;

static uint32_t clamp_tone_freq(uint32_t sample_rate_hz, uint32_t freq)
{
    if (sample_rate_hz == 0U) {
        sample_rate_hz = (uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ;
    }
    if (freq < 40U) {
        freq = 40U;
    }
    if (freq >= (sample_rate_hz / 2U)) {
        freq = sample_rate_hz / 4U;
    }
    return freq;
}

static void tone_set_frequency(uint32_t sample_rate_hz, uint32_t freq_hz)
{
    if (sample_rate_hz == 0U) {
        sample_rate_hz = (uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ;
    }
    uint32_t freq = clamp_tone_freq(sample_rate_hz, freq_hz);
    s_tone_inc_q32 = (uint32_t)(((uint64_t)freq << 32) / sample_rate_hz);
    if (s_tone_inc_q32 == 0U) {
        s_tone_inc_q32 = 1U;
    }
}

static void tone_reset(uint32_t sample_rate_hz, uint32_t freq_hz)
{
    s_tone_phase_q32 = 0U;
    tone_set_frequency(sample_rate_hz, freq_hz);
    s_last_tone_tick = xTaskGetTickCount();
}

static esp_err_t tone_write_chunk(uint32_t frame_count)
{
    uint32_t level_percent = (uint32_t)CONFIG_ORB_AUDIO_TEST_TONE_LEVEL_PERCENT;
    int32_t amplitude = (int32_t)((32767U * level_percent) / 100U);
    if (amplitude < 200) {
        amplitude = 200;
    }
    if (s_sim_tone_muted) {
        amplitude = 0;
    }

    uint32_t written = 0U;
    while (written < frame_count) {
        uint32_t chunk = frame_count - written;
        if (chunk > (uint32_t)(sizeof(s_tone_buffer) / sizeof(s_tone_buffer[0]))) {
            chunk = (uint32_t)(sizeof(s_tone_buffer) / sizeof(s_tone_buffer[0]));
        }

        for (uint32_t i = 0U; i < chunk; ++i) {
            s_tone_phase_q32 += s_tone_inc_q32;
            s_tone_buffer[i] = (s_tone_phase_q32 & 0x80000000U) ? (int16_t)amplitude : (int16_t)(-amplitude);
        }

        esp_err_t err = audio_worker_write_mixed_output(s_tone_buffer, chunk, true);
        if (err != ESP_OK) {
            return err;
        }
        written += chunk;
    }
    return ESP_OK;
}

static void tone_pump(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t chunk_ticks = audio_worker_ms_to_ticks_min1((uint32_t)CONFIG_ORB_AUDIO_TEST_TONE_CHUNK_MS);
    uint32_t chunk_ms = (uint32_t)CONFIG_ORB_AUDIO_TEST_TONE_CHUNK_MS;
    uint32_t frames_per_chunk = ((uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ * chunk_ms) / 1000U;
    if (frames_per_chunk < 16U) {
        frames_per_chunk = 16U;
    }

    if ((now - s_last_tone_tick) >= chunk_ticks) {
        esp_err_t err = tone_write_chunk(frames_per_chunk);
        if (err != ESP_OK) {
            TickType_t log_gap = audio_worker_ms_to_ticks_min1(1000U);
            if (err != ESP_ERR_TIMEOUT && (now - s_last_tone_err_tick) >= log_gap) {
                s_last_tone_err_tick = now;
                ESP_LOGW(TAG, "test tone write failed: %s", esp_err_to_name(err));
            }
            s_last_tone_tick = now;
            return;
        }
        s_last_tone_tick = now;
    }
}

static void simulated_melody_update(TickType_t playback_start_tick, TickType_t now)
{
    uint32_t elapsed_ms = (uint32_t)((now - playback_start_tick) * portTICK_PERIOD_MS);
    uint32_t step_ms = AUDIO_FALLBACK_MELODY_NOTE_MS + AUDIO_FALLBACK_MELODY_GAP_MS;
    uint32_t note_idx = (step_ms > 0U) ? (elapsed_ms / step_ms) : 0U;
    uint32_t in_note_ms = (step_ms > 0U) ? (elapsed_ms % step_ms) : 0U;
    uint32_t note_count = (uint32_t)(sizeof(s_fallback_melody_hz) / sizeof(s_fallback_melody_hz[0]));
    if (note_count == 0U || note_idx >= note_count) {
        return;
    }

    if ((uint8_t)note_idx != s_sim_note_index) {
        s_sim_note_index = (uint8_t)note_idx;
        tone_set_frequency((uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ, s_fallback_melody_hz[note_idx]);
    }
    s_sim_tone_muted = (in_note_ms >= AUDIO_FALLBACK_MELODY_NOTE_MS);
}
#endif

void audio_worker_sim_init(void)
{
#if CONFIG_ORB_AUDIO_TEST_TONE_ENABLE
    s_tone_phase_q32 = 0U;
    s_tone_inc_q32 = 0U;
    s_last_tone_tick = 0;
    s_last_tone_err_tick = 0;
    s_sim_note_index = 0U;
    s_sim_tone_muted = false;
#endif
}

void audio_worker_sim_start(void)
{
#if CONFIG_ORB_AUDIO_TEST_TONE_ENABLE
    s_sim_note_index = 0U;
    s_sim_tone_muted = false;
    if ((sizeof(s_fallback_melody_hz) / sizeof(s_fallback_melody_hz[0])) > 0U) {
        tone_reset((uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ, s_fallback_melody_hz[0]);
    }
    s_last_tone_err_tick = 0;
#endif
}

void audio_worker_sim_pump(TickType_t playback_start_tick)
{
#if CONFIG_ORB_AUDIO_TEST_TONE_ENABLE
    TickType_t now = xTaskGetTickCount();
    simulated_melody_update(playback_start_tick, now);
    tone_pump();
#else
    (void)playback_start_tick;
    (void)audio_worker_write_mixed_output(NULL, 0U, false);
#endif
}

uint32_t audio_worker_sim_duration_ms(uint32_t fallback_duration_ms)
{
    uint32_t note_count = (uint32_t)(sizeof(s_fallback_melody_hz) / sizeof(s_fallback_melody_hz[0]));
    uint32_t duration_ms = fallback_duration_ms;
    if (note_count > 0U) {
        uint32_t step_ms = AUDIO_FALLBACK_MELODY_NOTE_MS + AUDIO_FALLBACK_MELODY_GAP_MS;
        duration_ms = (note_count * AUDIO_FALLBACK_MELODY_NOTE_MS) + ((note_count - 1U) * AUDIO_FALLBACK_MELODY_GAP_MS);
        if (duration_ms < step_ms) {
            duration_ms = step_ms;
        }
    }
    return duration_ms;
}
