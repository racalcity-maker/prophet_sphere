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

typedef struct audio_worker_shared_state {
    bool output_started;
    bool output_paused;
    bool pcm_stream_active;
    uint8_t volume;
    TickType_t last_pcm_stream_timeout_log_tick;

    TickType_t audio_level_last_post_tick;
    uint8_t audio_level_filtered;
    uint8_t audio_level_last_sent;
    TickType_t audio_level_last_sent_tick;
    audio_reactive_analyzer_t reactive_analyzer;

    audio_bg_state_t bg;
    bool mp3_drop_first_frame;
    bool fg_content_started;
    bool fg_attack_active;
    uint32_t fg_attack_total_samples;
    uint32_t fg_attack_done_samples;
    int16_t mix_buffer[AUDIO_MIX_BUFFER_SAMPLES];
    int16_t bg_buffer[AUDIO_MIX_BUFFER_SAMPLES];
} audio_worker_shared_state_t;

/*
 * Internal audio_worker shared state type.
 * Instance ownership stays inside audio_worker.c; helper modules receive pointers
 * through explicit init/ctx APIs.
 */

TickType_t audio_worker_ms_to_ticks_min1(uint32_t ms);
uint32_t audio_worker_ms_to_samples(uint32_t ms);

#endif
