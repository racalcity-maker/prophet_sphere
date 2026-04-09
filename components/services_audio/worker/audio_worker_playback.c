#include "audio_worker_playback.h"

#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "audio_asset_registry.h"
#include "audio_worker_mixer.h"
#include "audio_worker_resampler.h"
#include "audio_worker_sim.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;

void audio_worker_playback_init(audio_worker_playback_ctx_t *ctx,
                                audio_worker_lifecycle_state_t *lifecycle,
                                audio_worker_shared_state_t *shared,
                                audio_mp3_helix_ctx_t *mp3,
                                uint32_t *playback_session_id,
                                TickType_t *playback_start_tick)
{
    if (ctx == NULL) {
        return;
    }
    (void)memset(ctx, 0, sizeof(*ctx));
    ctx->lifecycle = lifecycle;
    ctx->shared = shared;
    ctx->mp3 = mp3;
    ctx->playback_session_id = playback_session_id;
    ctx->playback_start_tick = playback_start_tick;
}

void audio_worker_playback_start_simulated(audio_worker_playback_ctx_t *ctx, uint32_t asset_id)
{
    if (ctx == NULL || ctx->lifecycle == NULL || ctx->shared == NULL ||
        ctx->playback_session_id == NULL || ctx->playback_start_tick == NULL) {
        return;
    }

    (*ctx->playback_session_id)++;
    if (*ctx->playback_session_id == 0U) {
        *ctx->playback_session_id = 1U;
    }

    ctx->lifecycle->active_asset_id = asset_id;
    ctx->lifecycle->playback_state = AUDIO_PLAYBACK_PLAYING;
    ctx->lifecycle->playback_source = AUDIO_PLAYBACK_SOURCE_SIMULATED;
    *ctx->playback_start_tick = xTaskGetTickCount();
    ctx->lifecycle->sim_duration_ms = audio_worker_sim_duration_ms((uint32_t)CONFIG_ORB_AUDIO_SIM_PLAYBACK_MS);
    ctx->shared->fg_content_started = true;
    audio_worker_fg_attack_reset(ctx->shared);
    audio_worker_sim_start();
    ESP_LOGI(TAG,
             "PLAY_ASSET id=%" PRIu32 " source=sim duration=%" PRIu32 "ms",
             ctx->lifecycle->active_asset_id,
             ctx->lifecycle->sim_duration_ms);
}

esp_err_t audio_worker_playback_start_mp3_or_fallback(audio_worker_playback_ctx_t *ctx,
                                                      uint32_t asset_id,
                                                      audio_worker_playback_ensure_output_fn_t ensure_output_started,
                                                      audio_worker_playback_bg_prefill_fn_t bg_prefill)
{
    if (ctx == NULL || ctx->lifecycle == NULL || ctx->shared == NULL || ctx->mp3 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->lifecycle->active_asset_id = asset_id;

#if CONFIG_ORB_AUDIO_REAL_MP3_ENABLE
    if (ctx->shared->bg.active && bg_prefill != NULL) {
        bg_prefill();
    }
    char path[384] = { 0 };
    esp_err_t reg_err = audio_asset_registry_resolve_path((audio_asset_id_t)asset_id, path, sizeof(path));
    if (reg_err == ESP_OK) {
        if (ctx->shared->bg.active && bg_prefill != NULL) {
            bg_prefill();
        }
        esp_err_t open_err = audio_mp3_helix_open(ctx->mp3, path);
        if (open_err == ESP_OK) {
            if (ctx->playback_session_id != NULL) {
                (*ctx->playback_session_id)++;
                if (*ctx->playback_session_id == 0U) {
                    *ctx->playback_session_id = 1U;
                }
            }
            ctx->lifecycle->playback_state = AUDIO_PLAYBACK_PLAYING;
            ctx->lifecycle->playback_source = AUDIO_PLAYBACK_SOURCE_MP3_HELIX;
            ctx->shared->mp3_drop_first_frame = true;
            ctx->shared->fg_content_started = false;
            audio_worker_resampler_reset();
            ESP_LOGI(TAG, "PLAY_ASSET id=%" PRIu32 " source=mp3 path=%s", ctx->lifecycle->active_asset_id, path);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "MP3 open failed (%s), fallback to sim", esp_err_to_name(open_err));
    } else {
        ESP_LOGW(TAG, "no MP3 path for asset id=%" PRIu32 ", fallback to sim", asset_id);
    }
#endif

    if (ensure_output_started == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t out_err = ensure_output_started();
    if (out_err != ESP_OK) {
        return out_err;
    }
    audio_worker_playback_start_simulated(ctx, asset_id);
    return ESP_OK;
}

audio_worker_playback_poll_result_t audio_worker_playback_poll(audio_worker_playback_ctx_t *ctx,
                                                               audio_worker_playback_ensure_output_fn_t ensure_output_started,
                                                               audio_worker_playback_pump_idle_fn_t pump_background_only)
{
    audio_worker_playback_poll_result_t result = { 0 };
    if (ctx == NULL || ctx->lifecycle == NULL || ctx->shared == NULL || ctx->mp3 == NULL) {
        result.error = true;
        result.error_code = ESP_ERR_INVALID_ARG;
        return result;
    }
    if (ctx->lifecycle->playback_state != AUDIO_PLAYBACK_PLAYING) {
        return result;
    }

    if (ctx->lifecycle->playback_source == AUDIO_PLAYBACK_SOURCE_SIMULATED) {
        uint32_t session_id = (ctx->playback_session_id != NULL) ? *ctx->playback_session_id : 0U;
        audio_worker_sim_pump((ctx->playback_start_tick != NULL) ? *ctx->playback_start_tick : 0U);

        if (ctx->playback_start_tick == NULL) {
            return result;
        }
        uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - *ctx->playback_start_tick) * portTICK_PERIOD_MS);
        if (elapsed_ms < ctx->lifecycle->sim_duration_ms) {
            return result;
        }
        if (ctx->playback_session_id != NULL && session_id != *ctx->playback_session_id) {
            return result;
        }
        if (ctx->lifecycle->playback_source != AUDIO_PLAYBACK_SOURCE_SIMULATED) {
            return result;
        }
        result.done = true;
        return result;
    }

    uint32_t session_id = (ctx->playback_session_id != NULL) ? *ctx->playback_session_id : 0U;
    const int16_t *samples = NULL;
    size_t sample_count = 0U;
    bool done = false;

    esp_err_t step_err = audio_mp3_helix_step(ctx->mp3, &samples, &sample_count, &done);
    if (step_err == ESP_ERR_NOT_FINISHED) {
        if (pump_background_only != NULL) {
            pump_background_only();
        }
        return result;
    }
    if (ctx->playback_session_id != NULL && session_id != *ctx->playback_session_id) {
        return result;
    }
    if (ctx->lifecycle->playback_source != AUDIO_PLAYBACK_SOURCE_MP3_HELIX) {
        return result;
    }
    if (step_err != ESP_OK) {
        result.error = true;
        result.error_code = step_err;
        return result;
    }

    if (sample_count > 0U && samples != NULL) {
        if (ctx->shared->mp3_drop_first_frame) {
            ctx->shared->mp3_drop_first_frame = false;
            if (done) {
                result.done = true;
            }
            return result;
        }

        if (ensure_output_started == NULL) {
            result.error = true;
            result.error_code = ESP_ERR_INVALID_ARG;
            return result;
        }
        esp_err_t out_err = ensure_output_started();
        if (out_err != ESP_OK) {
            result.error = true;
            result.error_code = out_err;
            return result;
        }
        uint32_t src_rate_hz = audio_mp3_helix_sample_rate_hz(ctx->mp3);
        esp_err_t wr_err = audio_worker_resampler_write_fg(ctx->shared, samples, sample_count, src_rate_hz);
        if (wr_err != ESP_OK && wr_err != ESP_ERR_TIMEOUT) {
            result.error = true;
            result.error_code = wr_err;
            return result;
        }
    }

    if (done) {
        result.done = true;
    }
    return result;
}
