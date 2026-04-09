#include "audio_worker.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "audio_mp3_helix.h"
#include "audio_output_i2s.h"
#include "audio_reactive_analyzer.h"
#include "audio_types.h"
#include "audio_worker_bg.h"
#include "audio_worker_bg_stream.h"
#include "audio_worker_commands.h"
#include "audio_worker_internal.h"
#include "audio_worker_lifecycle.h"
#include "audio_worker_mixer.h"
#include "audio_worker_playback.h"
#include "audio_worker_reactive.h"
#include "audio_worker_sim.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;

/* Local state shorthands (kept file-local on purpose). */
#define s_output_started s_shared_state.output_started
#define s_output_paused s_shared_state.output_paused
#define s_pcm_stream_active s_shared_state.pcm_stream_active
#define s_volume s_shared_state.volume
#define s_last_pcm_stream_timeout_log_tick s_shared_state.last_pcm_stream_timeout_log_tick
#define s_audio_level_last_post_tick s_shared_state.audio_level_last_post_tick
#define s_audio_level_filtered s_shared_state.audio_level_filtered
#define s_audio_level_last_sent s_shared_state.audio_level_last_sent
#define s_audio_level_last_sent_tick s_shared_state.audio_level_last_sent_tick
#define s_reactive_analyzer s_shared_state.reactive_analyzer
#define s_bg s_shared_state.bg
#define s_mp3_drop_first_frame s_shared_state.mp3_drop_first_frame
#define s_fg_content_started s_shared_state.fg_content_started
#define s_fg_attack_active s_shared_state.fg_attack_active
#define s_fg_attack_total_samples s_shared_state.fg_attack_total_samples
#define s_fg_attack_done_samples s_shared_state.fg_attack_done_samples
#define s_mix_buffer s_shared_state.mix_buffer
#define s_bg_buffer s_shared_state.bg_buffer

static audio_worker_lifecycle_state_t s_lifecycle;
static audio_worker_playback_ctx_t s_playback_ctx;
static audio_worker_commands_ctx_t s_commands_ctx;
static TaskHandle_t s_owner_task;
static uint32_t s_playback_session_id;

static audio_worker_shared_state_t s_shared_state;

static TickType_t s_playback_start_tick;

static TickType_t s_pcm_stream_diag_last_log_tick;
static uint32_t s_pcm_stream_rx_chunks;
static uint32_t s_pcm_stream_rx_samples;
static bool s_pcm_stream_chunk_written_since_poll;

static audio_mp3_helix_ctx_t s_mp3;

/* Lifecycle state shorthands (kept file-local on purpose). */
#define s_playback_state s_lifecycle.playback_state
#define s_playback_source s_lifecycle.playback_source
#define s_active_asset_id s_lifecycle.active_asset_id
#define s_pcm_stop_drain_pending s_lifecycle.pcm_stop_drain_pending

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

static void bg_prefill_dma_queue_cb(void)
{
    audio_worker_bg_stream_prefill_dma_queue(&s_shared_state);
}

static void pump_background_only_cb(void)
{
    audio_worker_bg_stream_pump_background_only(&s_shared_state);
}

static void finalize_with_done(void)
{
    assert_owner_task();
    uint32_t done_asset = s_active_asset_id;
    audio_worker_lifecycle_stop_foreground(&s_lifecycle, &s_shared_state, &s_mp3, false);
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
    audio_worker_lifecycle_stop_foreground(&s_lifecycle, &s_shared_state, &s_mp3, false);
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
    audio_worker_lifecycle_reset(&s_lifecycle);
    s_output_started = false;
    s_output_paused = false;
    s_volume = CONFIG_ORB_AUDIO_DEFAULT_VOLUME;
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
    audio_worker_bg_reset_state(&s_shared_state);
    s_mp3_drop_first_frame = false;
    s_fg_content_started = false;
    s_fg_attack_active = false;
    s_fg_attack_total_samples = 0U;
    s_fg_attack_done_samples = 0U;
    audio_worker_pcm_stream_diag_reset(&s_shared_state);
    (void)memset(s_mix_buffer, 0, sizeof(s_mix_buffer));
    (void)memset(s_bg_buffer, 0, sizeof(s_bg_buffer));
    audio_worker_playback_init(&s_playback_ctx,
                               &s_lifecycle,
                               &s_shared_state,
                               &s_mp3,
                               &s_playback_session_id,
                               &s_playback_start_tick);
    audio_worker_commands_init(&s_commands_ctx,
                               &s_lifecycle,
                               &s_shared_state,
                               &s_mp3,
                               &s_playback_ctx,
                               &s_pcm_stream_diag_last_log_tick,
                               &s_pcm_stream_rx_chunks,
                               &s_pcm_stream_rx_samples,
                               &s_pcm_stream_chunk_written_since_poll,
                               ensure_output_started,
                               bg_prefill_dma_queue_cb,
                               finalize_with_error);
    audio_worker_sim_init(&s_shared_state);
}

void audio_worker_handle_command(const audio_command_t *cmd)
{
    assert_owner_task();
    audio_worker_commands_handle(&s_commands_ctx, cmd);
}

void audio_worker_poll(void)
{
    assert_owner_task();

    if (s_playback_state == AUDIO_PLAYBACK_PLAYING) {
        audio_worker_playback_poll_result_t poll_result =
            audio_worker_playback_poll(&s_playback_ctx, ensure_output_started, pump_background_only_cb);
        if (poll_result.error) {
            ESP_LOGW(TAG, "playback poll failed: %s", esp_err_to_name((esp_err_t)poll_result.error_code));
            finalize_with_error(poll_result.error_code);
        } else if (poll_result.done) {
            if (s_playback_source == AUDIO_PLAYBACK_SOURCE_MP3_HELIX) {
                ESP_LOGI(TAG, "mp3 playback complete id=%" PRIu32, s_active_asset_id);
            } else {
                ESP_LOGI(TAG, "simulated playback complete id=%" PRIu32, s_active_asset_id);
            }
            finalize_with_done();
        }
    } else {
        /*
         * For PCM stream sessions foreground chunks are already mixed with BG in
         * AUDIO_CMD_PCM_STREAM_CHUNK path. Pumping standalone BG here adds an extra
         * blocking I2S write every loop and slows streamed speech. Still, when no
         * chunk arrived in current loop, we must pump BG to keep fade/progress alive.
         */
        if (s_pcm_stop_drain_pending) {
            audio_worker_lifecycle_pump_pcm_stop_drain(&s_lifecycle, &s_shared_state);
        } else if (!s_pcm_stream_active || !s_pcm_stream_chunk_written_since_poll) {
            pump_background_only_cb();
        }
    }
    s_pcm_stream_chunk_written_since_poll = false;

    audio_worker_audio_level_maybe_publish(&s_shared_state);
}

bool audio_worker_is_playing(void)
{
    assert_owner_task();
    return (s_playback_state == AUDIO_PLAYBACK_PLAYING) || s_bg.active || s_pcm_stream_active;
}
