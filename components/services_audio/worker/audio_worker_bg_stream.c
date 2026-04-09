#include "audio_worker_bg_stream.h"

#include <stdint.h>
#include "app_events.h"
#include "audio_worker_bg.h"
#include "audio_worker_mixer.h"
#include "audio_worker_reactive.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;

void audio_worker_bg_stream_prefill_dma_queue(audio_worker_shared_state_t *shared)
{
    if (shared == NULL) {
        return;
    }
    if (!shared->bg.active || !shared->output_started || shared->output_paused) {
        return;
    }

    for (uint32_t i = 0U; i < 64U; ++i) {
        esp_err_t wr_err = audio_worker_write_mixed_output(shared, NULL, 0U, false);
        if (wr_err == ESP_ERR_TIMEOUT) {
            break;
        }
        if (wr_err != ESP_OK) {
            ESP_LOGW(TAG, "background prefill failed: %s", esp_err_to_name(wr_err));
            break;
        }
    }
}

void audio_worker_bg_stream_close_with_fade_done_if_needed(audio_worker_shared_state_t *shared, const char *reason)
{
    if (shared == NULL) {
        return;
    }
    bool post_done = (shared->bg.active && shared->bg.fade_post_done_event);
    audio_worker_bg_close(shared);
    if (post_done) {
        ESP_LOGW(TAG, "background closed during fade (%s), posting synthetic fade-complete", reason);
        (void)audio_worker_post_audio_done(0U, (int32_t)APP_AUDIO_DONE_CODE_BG_FADE_COMPLETE);
    }
}

void audio_worker_bg_stream_pump_background_only(audio_worker_shared_state_t *shared)
{
    if (shared == NULL) {
        return;
    }

    /* Keep output fed not only for BG, but also for active PCM stream idle gaps.
     * Without this, no-BG streamed TTS can briefly hold/repeat last DMA fragment. */
    if (!shared->bg.active && !shared->pcm_stream_active) {
        return;
    }
    esp_err_t wr_err = audio_worker_write_mixed_output(shared, NULL, 0U, false);
    if (wr_err != ESP_OK && wr_err != ESP_ERR_TIMEOUT) {
        if (shared->bg.active) {
            ESP_LOGW(TAG, "background write failed: %s", esp_err_to_name(wr_err));
            audio_worker_bg_stream_close_with_fade_done_if_needed(shared, "write_failed");
        } else {
            ESP_LOGW(TAG, "pcm idle silence write failed: %s", esp_err_to_name(wr_err));
        }
    }
}
