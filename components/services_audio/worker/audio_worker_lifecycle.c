#include "audio_worker_lifecycle.h"

#include "sdkconfig.h"
#include "audio_output_i2s.h"
#include "audio_worker_bg.h"
#include "audio_worker_mixer.h"
#include "audio_worker_reactive.h"
#include "audio_worker_resampler.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;
static const int16_t s_silence_chunk_256[256] = { 0 };

static void close_mp3_if_open(audio_worker_lifecycle_state_t *lifecycle, audio_mp3_helix_ctx_t *mp3)
{
    if (lifecycle == NULL || mp3 == NULL) {
        return;
    }
    if (lifecycle->playback_source == AUDIO_PLAYBACK_SOURCE_MP3_HELIX) {
        audio_mp3_helix_close(mp3);
    }
}

static uint32_t pcm_tail_drain_target_samples(void)
{
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
    return samples_to_flush;
}

void audio_worker_lifecycle_reset(audio_worker_lifecycle_state_t *lifecycle)
{
    if (lifecycle == NULL) {
        return;
    }
    lifecycle->playback_state = AUDIO_PLAYBACK_IDLE;
    lifecycle->playback_source = AUDIO_PLAYBACK_SOURCE_NONE;
    lifecycle->active_asset_id = 0U;
    lifecycle->sim_duration_ms = (uint32_t)CONFIG_ORB_AUDIO_SIM_PLAYBACK_MS;
    lifecycle->pcm_stop_drain_pending = false;
    lifecycle->pcm_stop_drain_remaining_samples = 0U;
}

void audio_worker_lifecycle_stop_foreground(audio_worker_lifecycle_state_t *lifecycle,
                                            audio_worker_shared_state_t *shared,
                                            audio_mp3_helix_ctx_t *mp3,
                                            bool post_zero_level)
{
    if (lifecycle == NULL || shared == NULL) {
        return;
    }

    close_mp3_if_open(lifecycle, mp3);
    lifecycle->playback_state = AUDIO_PLAYBACK_IDLE;
    lifecycle->playback_source = AUDIO_PLAYBACK_SOURCE_NONE;
    lifecycle->active_asset_id = 0U;
    shared->mp3_drop_first_frame = false;
    shared->fg_content_started = false;
    shared->fg_attack_active = false;
    shared->fg_attack_total_samples = 0U;
    shared->fg_attack_done_samples = 0U;
    lifecycle->sim_duration_ms = (uint32_t)CONFIG_ORB_AUDIO_SIM_PLAYBACK_MS;
    audio_worker_resampler_reset();
    shared->pcm_stream_active = false;
    lifecycle->pcm_stop_drain_pending = false;
    lifecycle->pcm_stop_drain_remaining_samples = 0U;
    audio_worker_pcm_stream_diag_reset(shared);

    if (!shared->bg.active && shared->output_started && !shared->output_paused) {
        (void)audio_output_i2s_pause_stream();
        shared->output_paused = true;
    }
    audio_worker_audio_level_reset(shared, post_zero_level);
}

void audio_worker_lifecycle_stop_all(audio_worker_lifecycle_state_t *lifecycle,
                                     audio_worker_shared_state_t *shared,
                                     audio_mp3_helix_ctx_t *mp3)
{
    if (lifecycle == NULL || shared == NULL) {
        return;
    }

    audio_worker_lifecycle_stop_foreground(lifecycle, shared, mp3, false);
    audio_worker_bg_close(shared);
    shared->pcm_stream_active = false;
    if (shared->output_started && !shared->output_paused) {
        (void)audio_output_i2s_pause_stream();
        shared->output_paused = true;
    }
    audio_worker_audio_level_reset(shared, true);
}

void audio_worker_lifecycle_begin_pcm_stop_drain_if_needed(audio_worker_lifecycle_state_t *lifecycle,
                                                           const audio_worker_shared_state_t *shared)
{
    if (lifecycle == NULL || shared == NULL) {
        return;
    }

    if (!shared->output_started ||
        shared->output_paused ||
        shared->bg.active ||
        lifecycle->playback_state != AUDIO_PLAYBACK_IDLE) {
        lifecycle->pcm_stop_drain_pending = false;
        lifecycle->pcm_stop_drain_remaining_samples = 0U;
        return;
    }

    lifecycle->pcm_stop_drain_pending = true;
    lifecycle->pcm_stop_drain_remaining_samples = pcm_tail_drain_target_samples();
}

void audio_worker_lifecycle_pump_pcm_stop_drain(audio_worker_lifecycle_state_t *lifecycle,
                                                audio_worker_shared_state_t *shared)
{
    if (lifecycle == NULL || shared == NULL) {
        return;
    }
    if (!lifecycle->pcm_stop_drain_pending) {
        return;
    }
    if (!shared->output_started ||
        shared->output_paused ||
        shared->bg.active ||
        lifecycle->playback_state != AUDIO_PLAYBACK_IDLE) {
        lifecycle->pcm_stop_drain_pending = false;
        lifecycle->pcm_stop_drain_remaining_samples = 0U;
        return;
    }

    uint16_t chunk = 256U;
    if (lifecycle->pcm_stop_drain_remaining_samples < (uint32_t)chunk) {
        chunk = (uint16_t)lifecycle->pcm_stop_drain_remaining_samples;
    }
    esp_err_t err = audio_output_i2s_write_mono_pcm16(s_silence_chunk_256, chunk, 5U);
    if (err != ESP_OK) {
        if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGD(TAG, "pcm stop drain write failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (lifecycle->pcm_stop_drain_remaining_samples > (uint32_t)chunk) {
        lifecycle->pcm_stop_drain_remaining_samples -= (uint32_t)chunk;
        return;
    }

    lifecycle->pcm_stop_drain_remaining_samples = 0U;
    lifecycle->pcm_stop_drain_pending = false;
    (void)audio_output_i2s_pause_stream();
    shared->output_paused = true;
}
