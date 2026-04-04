#include "audio_worker.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "audio_asset_registry.h"
#include "audio_mp3_helix.h"
#include "audio_output_i2s.h"
#include "audio_reactive_analyzer.h"
#include "audio_types.h"
#include "audio_worker_bg.h"
#include "audio_worker_internal.h"
#include "audio_worker_mixer.h"
#include "audio_worker_reactive.h"
#include "audio_worker_sim.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;

typedef enum {
    AUDIO_PLAYBACK_SOURCE_NONE = 0,
    AUDIO_PLAYBACK_SOURCE_SIMULATED,
    AUDIO_PLAYBACK_SOURCE_MP3_HELIX,
} audio_playback_source_t;

static audio_playback_state_t s_playback_state = AUDIO_PLAYBACK_IDLE;
static audio_playback_source_t s_playback_source = AUDIO_PLAYBACK_SOURCE_NONE;
static TaskHandle_t s_owner_task;
static uint32_t s_playback_session_id;

bool s_output_started;
bool s_output_paused;
bool s_pcm_stream_active;
uint8_t s_volume;
static uint32_t s_active_asset_id;
static TickType_t s_playback_start_tick;
TickType_t s_last_pcm_stream_timeout_log_tick;
static TickType_t s_pcm_stream_diag_last_log_tick;
static uint32_t s_pcm_stream_rx_chunks;
static uint32_t s_pcm_stream_rx_samples;
static bool s_pcm_stream_chunk_written_since_poll;
TickType_t s_audio_level_last_post_tick;
uint8_t s_audio_level_filtered;
uint8_t s_audio_level_last_sent;
TickType_t s_audio_level_last_sent_tick;
audio_reactive_analyzer_t s_reactive_analyzer;
static audio_mp3_helix_ctx_t s_mp3;
audio_bg_state_t s_bg;
bool s_mp3_drop_first_frame;
bool s_fg_content_started;
bool s_fg_attack_active;
uint32_t s_fg_attack_total_samples;
uint32_t s_fg_attack_done_samples;
static uint32_t s_fg_src_rate_hz;
static uint32_t s_rs_step_q16;
static uint32_t s_rs_phase_q16;
static int16_t s_rs_prev_sample;
static bool s_rs_prev_valid;
int16_t s_mix_buffer[AUDIO_MIX_BUFFER_SAMPLES];
int16_t s_bg_buffer[AUDIO_MIX_BUFFER_SAMPLES];
static int16_t s_rs_out_buffer[AUDIO_MIX_BUFFER_SAMPLES];
static uint32_t s_sim_duration_ms;

TickType_t audio_worker_ms_to_ticks_min1(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks > 0) ? ticks : 1;
}

uint32_t audio_worker_ms_to_samples(uint32_t ms)
{
    uint64_t samples = ((uint64_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ * (uint64_t)ms) / 1000ULL;
    if (samples == 0ULL && ms > 0U) {
        return 1U;
    }
    if (samples > (uint64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)samples;
}

static inline void assert_owner_task(void)
{
    if (s_owner_task == NULL) {
        return;
    }
    configASSERT(xTaskGetCurrentTaskHandle() == s_owner_task);
}

static esp_err_t ensure_output_started(void)
{
    if (!s_output_started) {
        esp_err_t err = audio_output_i2s_start();
        if (err == ESP_OK) {
            s_output_started = true;
            s_output_paused = false;
        }
        return err;
    }
    if (s_output_paused) {
        esp_err_t err = audio_output_i2s_resume_stream();
        if (err == ESP_OK) {
            s_output_paused = false;
        }
        return err;
    }
    return ESP_OK;
}

static void close_mp3_if_open(void)
{
    if (s_playback_source == AUDIO_PLAYBACK_SOURCE_MP3_HELIX) {
        audio_mp3_helix_close(&s_mp3);
    }
}

static void resampler_reset(void)
{
    s_fg_src_rate_hz = 0U;
    s_rs_step_q16 = 0U;
    s_rs_phase_q16 = 0U;
    s_rs_prev_sample = 0;
    s_rs_prev_valid = false;
}

static void resampler_set_source_rate(uint32_t src_rate_hz)
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

static esp_err_t write_resampled_fg(const int16_t *samples, size_t sample_count, uint32_t src_rate_hz)
{
    if (samples == NULL || sample_count == 0U) {
        return ESP_OK;
    }

    if (src_rate_hz == 0U || src_rate_hz == (uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ) {
        return audio_worker_write_mixed_output(samples, sample_count, true);
    }

    resampler_set_source_rate(src_rate_hz);

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
                esp_err_t wr_err = audio_worker_write_mixed_output(s_rs_out_buffer, out_count, true);
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
        return audio_worker_write_mixed_output(s_rs_out_buffer, out_count, true);
    }
    return ESP_OK;
}

static void bg_prefill_dma_queue(void)
{
    if (!s_bg.active || !s_output_started || s_output_paused) {
        return;
    }

    for (uint32_t i = 0U; i < 64U; ++i) {
        esp_err_t wr_err = audio_worker_write_mixed_output(NULL, 0U, false);
        if (wr_err == ESP_ERR_TIMEOUT) {
            break;
        }
        if (wr_err != ESP_OK) {
            ESP_LOGW(TAG, "background prefill failed: %s", esp_err_to_name(wr_err));
            break;
        }
    }
}

static void bg_close_with_fade_done_if_needed(const char *reason)
{
    bool post_done = (s_bg.active && s_bg.fade_post_done_event);
    audio_worker_bg_close();
    if (post_done) {
        ESP_LOGW(TAG, "background closed during fade (%s), posting synthetic fade-complete", reason);
        (void)audio_worker_post_audio_done(0U, (int32_t)APP_AUDIO_DONE_CODE_BG_FADE_COMPLETE);
    }
}

static void stop_foreground(bool post_zero_level)
{
    close_mp3_if_open();
    s_playback_state = AUDIO_PLAYBACK_IDLE;
    s_playback_source = AUDIO_PLAYBACK_SOURCE_NONE;
    s_active_asset_id = 0U;
    s_mp3_drop_first_frame = false;
    s_fg_content_started = false;
    s_fg_attack_active = false;
    s_fg_attack_total_samples = 0U;
    s_fg_attack_done_samples = 0U;
    s_sim_duration_ms = (uint32_t)CONFIG_ORB_AUDIO_SIM_PLAYBACK_MS;
    resampler_reset();
    s_pcm_stream_active = false;

    if (!s_bg.active && s_output_started && !s_output_paused) {
        (void)audio_output_i2s_pause_stream();
        s_output_paused = true;
    }
    audio_worker_audio_level_reset(post_zero_level);
}

static void stop_all_playback(void)
{
    stop_foreground(false);
    audio_worker_bg_close();
    s_pcm_stream_active = false;
    if (s_output_started && !s_output_paused) {
        (void)audio_output_i2s_pause_stream();
        s_output_paused = true;
    }
    audio_worker_audio_level_reset(true);
}

static void flush_pcm_tail_silence(void)
{
    if (!s_output_started || s_output_paused) {
        return;
    }
    if (s_bg.active || s_playback_state != AUDIO_PLAYBACK_IDLE) {
        return;
    }
    static const int16_t kSilence[256] = { 0 };

    uint32_t dma_desc = (uint32_t)CONFIG_ORB_AUDIO_I2S_DMA_DESC_NUM;
    uint32_t dma_frame = (uint32_t)CONFIG_ORB_AUDIO_I2S_DMA_FRAME_NUM;
#if CONFIG_ORB_AUDIO_REAL_MP3_ENABLE
    if (dma_desc < 16U) {
        dma_desc = 16U;
    }
    if (dma_frame < 512U) {
        dma_frame = 512U;
    }
#endif
    uint32_t samples_to_flush = dma_desc * dma_frame;
    if (samples_to_flush < 1024U) {
        samples_to_flush = 1024U;
    }
    samples_to_flush += dma_frame; /* one extra frame as guard */

    uint32_t sent = 0U;
    while (sent < samples_to_flush) {
        uint16_t chunk = 256U;
        uint32_t left = samples_to_flush - sent;
        if (left < (uint32_t)chunk) {
            chunk = (uint16_t)left;
        }
        esp_err_t err = audio_output_i2s_write_mono_pcm16(kSilence, chunk, 20U);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "pcm tail flush interrupted at %lu/%lu samples: %s",
                     (unsigned long)sent,
                     (unsigned long)samples_to_flush,
                     esp_err_to_name(err));
            break;
        }
        sent += chunk;
    }

    uint32_t drain_ms = (uint32_t)(((uint64_t)sent * 1000ULL) / (uint64_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ);
    if (drain_ms < 8U) {
        drain_ms = 8U;
    }
    if (drain_ms > 260U) {
        drain_ms = 260U;
    }
    vTaskDelay(audio_worker_ms_to_ticks_min1(drain_ms));
}

static void finalize_with_done(void)
{
    assert_owner_task();
    uint32_t done_asset = s_active_asset_id;
    stop_foreground(false);
    if (audio_worker_post_audio_done(done_asset, (int32_t)APP_AUDIO_DONE_CODE_NONE) == ESP_OK) {
        ESP_LOGI(TAG, "posted APP_EVENT_AUDIO_DONE id=%" PRIu32, done_asset);
    } else {
        ESP_LOGW(TAG, "failed to post APP_EVENT_AUDIO_DONE id=%" PRIu32, done_asset);
    }
}

static void finalize_with_error(int32_t code)
{
    assert_owner_task();
    uint32_t err_asset = s_active_asset_id;
    stop_foreground(false);
    if (audio_worker_post_audio_error(err_asset, code) == ESP_OK) {
        ESP_LOGW(TAG, "posted APP_EVENT_AUDIO_ERROR id=%" PRIu32 " code=%" PRId32, err_asset, code);
    } else {
        ESP_LOGW(TAG, "failed to post APP_EVENT_AUDIO_ERROR id=%" PRIu32 " code=%" PRId32, err_asset, code);
    }
}

void audio_worker_init(void)
{
    s_owner_task = xTaskGetCurrentTaskHandle();
    s_playback_session_id = 0U;
    s_output_started = false;
    s_output_paused = false;
    s_playback_state = AUDIO_PLAYBACK_IDLE;
    s_playback_source = AUDIO_PLAYBACK_SOURCE_NONE;
    s_volume = CONFIG_ORB_AUDIO_DEFAULT_VOLUME;
    s_active_asset_id = 0;
    s_playback_start_tick = 0;
    s_last_pcm_stream_timeout_log_tick = 0;
    s_pcm_stream_diag_last_log_tick = 0;
    s_pcm_stream_rx_chunks = 0U;
    s_pcm_stream_rx_samples = 0U;
    s_pcm_stream_chunk_written_since_poll = false;
    s_audio_level_last_post_tick = 0;
    s_audio_level_filtered = 0U;
    s_audio_level_last_sent = 0U;
    s_audio_level_last_sent_tick = 0;
    audio_reactive_analyzer_init(&s_reactive_analyzer);
    memset(&s_mp3, 0, sizeof(s_mp3));
    audio_worker_bg_reset_state();
    s_mp3_drop_first_frame = false;
    s_fg_content_started = false;
    s_fg_attack_active = false;
    s_fg_attack_total_samples = 0U;
    s_fg_attack_done_samples = 0U;
    resampler_reset();
    (void)memset(s_mix_buffer, 0, sizeof(s_mix_buffer));
    (void)memset(s_bg_buffer, 0, sizeof(s_bg_buffer));
    (void)memset(s_rs_out_buffer, 0, sizeof(s_rs_out_buffer));
    audio_worker_sim_init();
}

static void start_simulated_playback(uint32_t asset_id)
{
    assert_owner_task();
    s_playback_session_id++;
    if (s_playback_session_id == 0U) {
        s_playback_session_id = 1U;
    }
    s_active_asset_id = asset_id;
    s_playback_state = AUDIO_PLAYBACK_PLAYING;
    s_playback_source = AUDIO_PLAYBACK_SOURCE_SIMULATED;
    s_playback_start_tick = xTaskGetTickCount();
    s_sim_duration_ms = audio_worker_sim_duration_ms((uint32_t)CONFIG_ORB_AUDIO_SIM_PLAYBACK_MS);
    s_fg_content_started = true;
    audio_worker_fg_attack_reset();
    audio_worker_sim_start();
    ESP_LOGI(TAG,
             "PLAY_ASSET id=%" PRIu32 " source=sim duration=%" PRIu32 "ms",
             s_active_asset_id,
             s_sim_duration_ms);
}

static void start_mp3_or_fallback(uint32_t asset_id)
{
    assert_owner_task();
    s_active_asset_id = asset_id;

#if CONFIG_ORB_AUDIO_REAL_MP3_ENABLE
    if (s_bg.active) {
        bg_prefill_dma_queue();
    }
    char path[384] = { 0 };
    esp_err_t reg_err = audio_asset_registry_resolve_path((audio_asset_id_t)asset_id, path, sizeof(path));
    if (reg_err == ESP_OK) {
        if (s_bg.active) {
            bg_prefill_dma_queue();
        }
        esp_err_t open_err = audio_mp3_helix_open(&s_mp3, path);
        if (open_err == ESP_OK) {
            s_playback_session_id++;
            if (s_playback_session_id == 0U) {
                s_playback_session_id = 1U;
            }
            s_playback_state = AUDIO_PLAYBACK_PLAYING;
            s_playback_source = AUDIO_PLAYBACK_SOURCE_MP3_HELIX;
            s_mp3_drop_first_frame = true;
            s_fg_content_started = false;
            resampler_reset();
            ESP_LOGI(TAG, "PLAY_ASSET id=%" PRIu32 " source=mp3 path=%s", s_active_asset_id, path);
            return;
        }
        ESP_LOGW(TAG, "MP3 open failed (%s), fallback to sim", esp_err_to_name(open_err));
    } else {
        ESP_LOGW(TAG, "no MP3 path for asset id=%" PRIu32 ", fallback to sim", asset_id);
    }
#endif

    esp_err_t out_err = ensure_output_started();
    if (out_err != ESP_OK) {
        ESP_LOGW(TAG, "audio output start failed: %s", esp_err_to_name(out_err));
        finalize_with_error(out_err);
        return;
    }
    start_simulated_playback(asset_id);
}

void audio_worker_handle_command(const audio_command_t *cmd)
{
    assert_owner_task();
    if (cmd == NULL) {
        return;
    }

    switch (cmd->id) {
    case AUDIO_CMD_PLAY_ASSET:
        s_pcm_stream_active = false;
        stop_foreground(false);
        start_mp3_or_fallback(cmd->payload.play_asset.asset_id);
        break;
    case AUDIO_CMD_STOP:
        ESP_LOGI(TAG, "STOP");
        stop_all_playback();
        break;
    case AUDIO_CMD_SET_VOLUME:
        s_volume = cmd->payload.set_volume.volume;
        ESP_LOGI(TAG, "SET_VOLUME %u", (unsigned)s_volume);
        break;
    case AUDIO_CMD_BG_START: {
        s_pcm_stream_active = false;
        esp_err_t out_err = ensure_output_started();
        if (out_err != ESP_OK) {
            ESP_LOGW(TAG, "audio output start for background failed: %s", esp_err_to_name(out_err));
            break;
        }
        uint16_t gain = cmd->payload.bg_start.gain_permille;
        if (gain == 0U) {
            gain = (uint16_t)CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE;
        }
        esp_err_t bg_err = audio_worker_bg_start(cmd->payload.bg_start.fade_in_ms, gain);
        if (bg_err != ESP_OK) {
            ESP_LOGW(TAG, "background start failed: %s", esp_err_to_name(bg_err));
        }
        break;
    }
    case AUDIO_CMD_BG_SET_GAIN: {
        uint16_t gain = cmd->payload.bg_set_gain.gain_permille;
        if (gain > 1000U) {
            gain = 1000U;
        }
        if (!s_bg.active) {
            esp_err_t out_err = ensure_output_started();
            if (out_err != ESP_OK) {
                ESP_LOGW(TAG, "background set gain fallback start failed to ensure output: %s", esp_err_to_name(out_err));
                break;
            }
            esp_err_t bg_err = audio_worker_bg_start(cmd->payload.bg_set_gain.fade_ms, gain);
            if (bg_err != ESP_OK) {
                ESP_LOGW(TAG, "background set gain fallback start failed: %s", esp_err_to_name(bg_err));
            } else {
                ESP_LOGI(TAG, "background set gain fallback start fade=%" PRIu32 "ms gain=%u",
                         cmd->payload.bg_set_gain.fade_ms,
                         (unsigned)gain);
            }
            break;
        }
        audio_worker_bg_begin_fade(gain, cmd->payload.bg_set_gain.fade_ms, false);
        ESP_LOGI(TAG, "background set gain fade=%" PRIu32 "ms gain=%u",
                 cmd->payload.bg_set_gain.fade_ms,
                 (unsigned)gain);
        break;
    }
    case AUDIO_CMD_BG_FADE_OUT:
        if (s_bg.active) {
            if (!s_output_started || s_output_paused) {
                ESP_LOGW(TAG, "background fade-out requested while output is not running, forcing immediate done");
                audio_worker_bg_close();
                (void)audio_worker_post_audio_done(0U, (int32_t)APP_AUDIO_DONE_CODE_BG_FADE_COMPLETE);
            } else {
                audio_worker_bg_begin_fade(0U, cmd->payload.bg_fade_out.fade_out_ms, true);
                ESP_LOGI(TAG, "background fade-out %" PRIu32 "ms", cmd->payload.bg_fade_out.fade_out_ms);
            }
        } else {
            ESP_LOGW(TAG, "background fade-out requested with no active background, posting immediate done");
            (void)audio_worker_post_audio_done(0U, (int32_t)APP_AUDIO_DONE_CODE_BG_FADE_COMPLETE);
        }
        break;
    case AUDIO_CMD_BG_STOP:
        audio_worker_bg_close();
        s_pcm_stream_active = false;
        if (s_playback_state == AUDIO_PLAYBACK_IDLE && s_output_started && !s_output_paused) {
            (void)audio_output_i2s_pause_stream();
            s_output_paused = true;
            audio_worker_audio_level_reset(true);
        }
        break;
    case AUDIO_CMD_PCM_STREAM_START: {
        stop_foreground(true);
        esp_err_t out_err = ensure_output_started();
        if (out_err != ESP_OK) {
            ESP_LOGW(TAG, "audio output start for pcm stream failed: %s", esp_err_to_name(out_err));
            break;
        }
        s_pcm_stream_active = true;
        s_pcm_stream_diag_last_log_tick = xTaskGetTickCount();
        s_pcm_stream_rx_chunks = 0U;
        s_pcm_stream_rx_samples = 0U;
        s_pcm_stream_chunk_written_since_poll = false;
        ESP_LOGW(TAG, "PCM stream start");
        break;
    }
    case AUDIO_CMD_PCM_STREAM_STOP:
        s_pcm_stream_active = false;
        s_pcm_stream_chunk_written_since_poll = false;
        if (!s_bg.active && s_playback_state == AUDIO_PLAYBACK_IDLE && s_output_started && !s_output_paused) {
            flush_pcm_tail_silence();
            (void)audio_output_i2s_pause_stream();
            s_output_paused = true;
        }
        audio_worker_audio_level_reset(true);
        ESP_LOGW(TAG,
                 "PCM stream stop chunks=%" PRIu32 " samples=%" PRIu32,
                 s_pcm_stream_rx_chunks,
                 s_pcm_stream_rx_samples);
        break;
    case AUDIO_CMD_PCM_STREAM_CHUNK:
        if (!s_pcm_stream_active) {
            break;
        }
        if (cmd->payload.pcm_stream_chunk.sample_count == 0U ||
            cmd->payload.pcm_stream_chunk.sample_count > AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES) {
            break;
        }
        if (ensure_output_started() != ESP_OK) {
            break;
        }
        {
            s_pcm_stream_rx_chunks++;
            s_pcm_stream_rx_samples += (uint32_t)cmd->payload.pcm_stream_chunk.sample_count;
            TickType_t now = xTaskGetTickCount();
            TickType_t diag_gap = audio_worker_ms_to_ticks_min1(1000U);
            if ((now - s_pcm_stream_diag_last_log_tick) >= diag_gap) {
                s_pcm_stream_diag_last_log_tick = now;
                uint32_t audio_ms = (uint32_t)(((uint64_t)s_pcm_stream_rx_samples * 1000ULL) /
                                               (uint64_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ);
                ESP_LOGD(TAG,
                         "pcm stream rx diag chunks=%" PRIu32 " samples=%" PRIu32
                         " audio_ms=%" PRIu32 " bg=%u",
                         s_pcm_stream_rx_chunks,
                         s_pcm_stream_rx_samples,
                         audio_ms,
                         s_bg.active ? 1U : 0U);
            }
            esp_err_t wr_err = audio_worker_write_mixed_output(cmd->payload.pcm_stream_chunk.samples,
                                                               (size_t)cmd->payload.pcm_stream_chunk.sample_count,
                                                               true);
            if (wr_err == ESP_ERR_TIMEOUT) {
                TickType_t now = xTaskGetTickCount();
                TickType_t gap = audio_worker_ms_to_ticks_min1(1000U);
                if ((now - s_last_pcm_stream_timeout_log_tick) >= gap) {
                    s_last_pcm_stream_timeout_log_tick = now;
                    ESP_LOGW(TAG, "PCM stream write timeout");
                }
            } else if (wr_err != ESP_OK) {
                ESP_LOGW(TAG, "PCM stream write failed: %s", esp_err_to_name(wr_err));
            } else {
                s_pcm_stream_chunk_written_since_poll = true;
            }
        }
        break;
    case AUDIO_CMD_NONE:
    default:
        ESP_LOGW(TAG, "unknown command id=%d", (int)cmd->id);
        break;
    }
}

static void poll_simulated(void)
{
    assert_owner_task();
    uint32_t session_id = s_playback_session_id;
    audio_worker_sim_pump(s_playback_start_tick);

    uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - s_playback_start_tick) * portTICK_PERIOD_MS);
    if (elapsed_ms < s_sim_duration_ms) {
        return;
    }
    if (session_id != s_playback_session_id || s_playback_source != AUDIO_PLAYBACK_SOURCE_SIMULATED) {
        return;
    }

    ESP_LOGI(TAG, "simulated playback complete id=%" PRIu32, s_active_asset_id);
    finalize_with_done();
}

static void pump_background_only(void)
{
    if (!s_bg.active) {
        return;
    }
    esp_err_t wr_err = audio_worker_write_mixed_output(NULL, 0U, false);
    if (wr_err != ESP_OK && wr_err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "background write failed: %s", esp_err_to_name(wr_err));
        bg_close_with_fade_done_if_needed("write_failed");
    }
}

static void poll_mp3(void)
{
    assert_owner_task();
    uint32_t session_id = s_playback_session_id;
    const int16_t *samples = NULL;
    size_t sample_count = 0;
    bool done = false;

    esp_err_t step_err = audio_mp3_helix_step(&s_mp3, &samples, &sample_count, &done);
    if (step_err == ESP_ERR_NOT_FINISHED) {
        pump_background_only();
        return;
    }
    if (session_id != s_playback_session_id || s_playback_source != AUDIO_PLAYBACK_SOURCE_MP3_HELIX) {
        return;
    }
    if (step_err != ESP_OK) {
        ESP_LOGW(TAG, "MP3 decode failed: %s", esp_err_to_name(step_err));
        finalize_with_error(step_err);
        return;
    }

    if (sample_count > 0U && samples != NULL) {
        if (s_mp3_drop_first_frame) {
            s_mp3_drop_first_frame = false;
            if (done) {
                ESP_LOGI(TAG, "mp3 playback complete id=%" PRIu32, s_active_asset_id);
                finalize_with_done();
            }
            return;
        }

        esp_err_t out_err = ensure_output_started();
        if (out_err != ESP_OK) {
            ESP_LOGW(TAG, "audio output start failed before MP3 write: %s", esp_err_to_name(out_err));
            finalize_with_error(out_err);
            return;
        }
        uint32_t src_rate_hz = audio_mp3_helix_sample_rate_hz(&s_mp3);
        esp_err_t wr_err = write_resampled_fg(samples, sample_count, src_rate_hz);
        if (wr_err != ESP_OK && wr_err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "I2S write failed during MP3 playback: %s", esp_err_to_name(wr_err));
            finalize_with_error(wr_err);
            return;
        }
    }

    if (done) {
        ESP_LOGI(TAG, "mp3 playback complete id=%" PRIu32, s_active_asset_id);
        finalize_with_done();
    }
}

void audio_worker_poll(void)
{
    assert_owner_task();

    if (s_playback_state == AUDIO_PLAYBACK_PLAYING) {
        if (s_playback_source == AUDIO_PLAYBACK_SOURCE_MP3_HELIX) {
            poll_mp3();
        } else {
            poll_simulated();
        }
    } else {
        /*
         * For PCM stream sessions foreground chunks are already mixed with BG in
         * AUDIO_CMD_PCM_STREAM_CHUNK path. Pumping standalone BG here adds an extra
         * blocking I2S write every loop and slows streamed speech. Still, when no
         * chunk arrived in current loop, we must pump BG to keep fade/progress alive.
         */
        if (!s_pcm_stream_active || !s_pcm_stream_chunk_written_since_poll) {
            pump_background_only();
        }
    }
    s_pcm_stream_chunk_written_since_poll = false;

    audio_worker_audio_level_maybe_publish();
}

bool audio_worker_is_playing(void)
{
    assert_owner_task();
    return (s_playback_state == AUDIO_PLAYBACK_PLAYING) || s_bg.active || s_pcm_stream_active;
}
