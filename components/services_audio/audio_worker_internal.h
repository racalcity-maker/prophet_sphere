#ifndef AUDIO_WORKER_INTERNAL_H
#define AUDIO_WORKER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_reactive_analyzer.h"

#ifndef CONFIG_ORB_AUDIO_FG_ATTACK_MS
#define CONFIG_ORB_AUDIO_FG_ATTACK_MS 12
#endif
#ifndef CONFIG_ORB_AUDIO_FG_SIGNAL_ABS_THRESHOLD
#define CONFIG_ORB_AUDIO_FG_SIGNAL_ABS_THRESHOLD 24U
#endif

#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_WAV_PATH
#define CONFIG_ORB_AUDIO_PROPHECY_BG_WAV_PATH "/sdcard/audio/backgrounds/oracle.wav"
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE
#define CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE 260
#endif

#define AUDIO_MIX_BUFFER_SAMPLES 2048U
#define AUDIO_BG_ONLY_CHUNK_SAMPLES 512U

typedef struct {
    FILE *file;
    uint32_t data_offset;
    uint32_t data_size_bytes;
    uint32_t data_pos_bytes;
    uint32_t sample_rate_hz;
    uint16_t gain_permille;
    uint16_t fade_start_gain_permille;
    uint16_t fade_target_gain_permille;
    uint32_t fade_total_samples;
    uint32_t fade_done_samples;
    bool active;
    bool fade_active;
    bool fade_post_done_event;
} audio_bg_state_t;

/*
 * Internal audio_worker shared state.
 * Owned by audio_task/audio_worker thread; helper modules only access it from that context.
 */
extern bool s_output_started;
extern bool s_output_paused;
extern bool s_pcm_stream_active;
extern uint8_t s_volume;
extern TickType_t s_last_pcm_stream_timeout_log_tick;

extern TickType_t s_audio_level_last_post_tick;
extern uint8_t s_audio_level_filtered;
extern uint8_t s_audio_level_last_sent;
extern TickType_t s_audio_level_last_sent_tick;
extern audio_reactive_analyzer_t s_reactive_analyzer;

extern audio_bg_state_t s_bg;
extern bool s_mp3_drop_first_frame;
extern bool s_fg_content_started;
extern bool s_fg_attack_active;
extern uint32_t s_fg_attack_total_samples;
extern uint32_t s_fg_attack_done_samples;
extern int16_t s_mix_buffer[AUDIO_MIX_BUFFER_SAMPLES];
extern int16_t s_bg_buffer[AUDIO_MIX_BUFFER_SAMPLES];

TickType_t audio_worker_ms_to_ticks_min1(uint32_t ms);
uint32_t audio_worker_ms_to_samples(uint32_t ms);

#endif
