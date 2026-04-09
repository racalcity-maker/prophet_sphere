#include "audio_worker_commands.h"

#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "audio_asset_registry.h"
#include "audio_output_i2s.h"
#include "audio_worker_bg.h"
#include "audio_worker_mixer.h"
#include "audio_worker_reactive.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;

void audio_worker_commands_init(audio_worker_commands_ctx_t *ctx,
                                audio_worker_lifecycle_state_t *lifecycle,
                                audio_worker_shared_state_t *shared,
                                audio_mp3_helix_ctx_t *mp3,
                                audio_worker_playback_ctx_t *playback,
                                TickType_t *pcm_stream_diag_last_log_tick,
                                uint32_t *pcm_stream_rx_chunks,
                                uint32_t *pcm_stream_rx_samples,
                                bool *pcm_stream_chunk_written_since_poll,
                                audio_worker_playback_ensure_output_fn_t ensure_output_started,
                                audio_worker_playback_bg_prefill_fn_t bg_prefill,
                                audio_worker_commands_on_play_start_error_fn_t on_play_start_error)
{
    if (ctx == NULL) {
        return;
    }
    (void)memset(ctx, 0, sizeof(*ctx));
    ctx->lifecycle = lifecycle;
    ctx->shared = shared;
    ctx->mp3 = mp3;
    ctx->playback = playback;
    ctx->pcm_stream_diag_last_log_tick = pcm_stream_diag_last_log_tick;
    ctx->pcm_stream_rx_chunks = pcm_stream_rx_chunks;
    ctx->pcm_stream_rx_samples = pcm_stream_rx_samples;
    ctx->pcm_stream_chunk_written_since_poll = pcm_stream_chunk_written_since_poll;
    ctx->ensure_output_started = ensure_output_started;
    ctx->bg_prefill = bg_prefill;
    ctx->on_play_start_error = on_play_start_error;
}

void audio_worker_commands_handle(audio_worker_commands_ctx_t *ctx, const audio_command_t *cmd)
{
    if (ctx == NULL || ctx->lifecycle == NULL || ctx->shared == NULL || ctx->mp3 == NULL ||
        ctx->playback == NULL || ctx->pcm_stream_diag_last_log_tick == NULL ||
        ctx->pcm_stream_rx_chunks == NULL || ctx->pcm_stream_rx_samples == NULL ||
        ctx->pcm_stream_chunk_written_since_poll == NULL || ctx->ensure_output_started == NULL || cmd == NULL) {
        return;
    }

    switch (cmd->id) {
    case AUDIO_CMD_PLAY_ASSET:
        ctx->shared->pcm_stream_active = false;
        audio_worker_lifecycle_stop_foreground(ctx->lifecycle, ctx->shared, ctx->mp3, false);
        {
            esp_err_t start_err =
                audio_worker_playback_start_mp3_or_fallback(ctx->playback,
                                                            cmd->payload.play_asset.asset_id,
                                                            ctx->ensure_output_started,
                                                            ctx->bg_prefill);
            if (start_err != ESP_OK) {
                ESP_LOGW(TAG, "audio output start failed: %s", esp_err_to_name(start_err));
                if (ctx->on_play_start_error != NULL) {
                    ctx->on_play_start_error(start_err);
                }
            }
        }
        break;

    case AUDIO_CMD_STOP:
        ESP_LOGI(TAG, "STOP");
        audio_worker_lifecycle_stop_all(ctx->lifecycle, ctx->shared, ctx->mp3);
        break;

    case AUDIO_CMD_SET_VOLUME:
        ctx->shared->volume = cmd->payload.set_volume.volume;
        ESP_LOGI(TAG, "SET_VOLUME %u", (unsigned)ctx->shared->volume);
        break;

    case AUDIO_CMD_SET_DYNAMIC_ASSET_PATH: {
        const uint32_t slot_id = cmd->payload.set_dynamic_asset_path.slot_id;
        const char *path = cmd->payload.set_dynamic_asset_path.path;
        if (path == NULL || path[0] == '\0') {
            ESP_LOGW(TAG, "SET_DYNAMIC_ASSET_PATH invalid empty path");
            break;
        }
        esp_err_t dyn_err = audio_asset_registry_set_dynamic_path((audio_asset_id_t)slot_id, path);
        if (dyn_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "SET_DYNAMIC_ASSET_PATH failed slot=%" PRIu32 ": %s",
                     slot_id,
                     esp_err_to_name(dyn_err));
        }
        break;
    }

    case AUDIO_CMD_BG_START: {
        ctx->shared->pcm_stream_active = false;
        esp_err_t out_err = ctx->ensure_output_started();
        if (out_err != ESP_OK) {
            ESP_LOGW(TAG, "audio output start for background failed: %s", esp_err_to_name(out_err));
            break;
        }
        uint16_t gain = cmd->payload.bg_start.gain_permille;
        if (gain == 0U) {
            gain = (uint16_t)CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE;
        }
        esp_err_t bg_err = audio_worker_bg_start(ctx->shared, cmd->payload.bg_start.fade_in_ms, gain);
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
        if (!ctx->shared->bg.active) {
            esp_err_t out_err = ctx->ensure_output_started();
            if (out_err != ESP_OK) {
                ESP_LOGW(TAG, "background set gain fallback start failed to ensure output: %s", esp_err_to_name(out_err));
                break;
            }
            esp_err_t bg_err = audio_worker_bg_start(ctx->shared, cmd->payload.bg_set_gain.fade_ms, gain);
            if (bg_err != ESP_OK) {
                ESP_LOGW(TAG, "background set gain fallback start failed: %s", esp_err_to_name(bg_err));
            } else {
                ESP_LOGI(TAG, "background set gain fallback start fade=%" PRIu32 "ms gain=%u",
                         cmd->payload.bg_set_gain.fade_ms,
                         (unsigned)gain);
            }
            break;
        }
        audio_worker_bg_begin_fade(ctx->shared, gain, cmd->payload.bg_set_gain.fade_ms, false);
        ESP_LOGI(TAG, "background set gain fade=%" PRIu32 "ms gain=%u",
                 cmd->payload.bg_set_gain.fade_ms,
                 (unsigned)gain);
        break;
    }

    case AUDIO_CMD_BG_FADE_OUT:
        if (ctx->shared->bg.active) {
            if (!ctx->shared->output_started || ctx->shared->output_paused) {
                ESP_LOGW(TAG, "background fade-out requested while output is not running, forcing immediate done");
                audio_worker_bg_close(ctx->shared);
                (void)audio_worker_post_audio_done(0U, (int32_t)APP_AUDIO_DONE_CODE_BG_FADE_COMPLETE);
            } else {
                audio_worker_bg_begin_fade(ctx->shared, 0U, cmd->payload.bg_fade_out.fade_out_ms, true);
                ESP_LOGI(TAG, "background fade-out %" PRIu32 "ms", cmd->payload.bg_fade_out.fade_out_ms);
            }
        } else {
            ESP_LOGW(TAG, "background fade-out requested with no active background, posting immediate done");
            (void)audio_worker_post_audio_done(0U, (int32_t)APP_AUDIO_DONE_CODE_BG_FADE_COMPLETE);
        }
        break;

    case AUDIO_CMD_BG_STOP:
        audio_worker_bg_close(ctx->shared);
        ctx->shared->pcm_stream_active = false;
        if (ctx->lifecycle->playback_state == AUDIO_PLAYBACK_IDLE &&
            ctx->shared->output_started &&
            !ctx->shared->output_paused) {
            (void)audio_output_i2s_pause_stream();
            ctx->shared->output_paused = true;
            audio_worker_audio_level_reset(ctx->shared, true);
        }
        break;

    case AUDIO_CMD_PCM_STREAM_START: {
        audio_worker_lifecycle_stop_foreground(ctx->lifecycle, ctx->shared, ctx->mp3, true);
        esp_err_t out_err = ctx->ensure_output_started();
        if (out_err != ESP_OK) {
            ESP_LOGW(TAG, "audio output start for pcm stream failed: %s", esp_err_to_name(out_err));
            break;
        }
        ctx->shared->pcm_stream_active = true;
        *ctx->pcm_stream_diag_last_log_tick = xTaskGetTickCount();
        *ctx->pcm_stream_rx_chunks = 0U;
        *ctx->pcm_stream_rx_samples = 0U;
        *ctx->pcm_stream_chunk_written_since_poll = false;
        audio_worker_fg_attack_reset(ctx->shared);
        audio_worker_pcm_stream_diag_reset(ctx->shared);
        ESP_LOGW(TAG, "PCM stream start");
        break;
    }

    case AUDIO_CMD_PCM_STREAM_STOP: {
        uint32_t out_jump_count = 0U;
        uint32_t out_jump_max = 0U;
        audio_worker_pcm_stream_diag_snapshot(ctx->shared, &out_jump_count, &out_jump_max);
        ctx->shared->pcm_stream_active = false;
        *ctx->pcm_stream_chunk_written_since_poll = false;
        audio_worker_lifecycle_begin_pcm_stop_drain_if_needed(ctx->lifecycle, ctx->shared);
        audio_worker_pcm_stream_diag_reset(ctx->shared);
        audio_worker_audio_level_reset(ctx->shared, true);
        ESP_LOGW(TAG,
                 "PCM stream stop chunks=%" PRIu32 " samples=%" PRIu32
                 " out_jumps=%" PRIu32 " out_jump_max=%" PRIu32,
                 *ctx->pcm_stream_rx_chunks,
                 *ctx->pcm_stream_rx_samples,
                 out_jump_count,
                 out_jump_max);
        break;
    }

    case AUDIO_CMD_PCM_STREAM_CHUNK: {
        audio_pcm_chunk_t *chunk = cmd->payload.pcm_stream_chunk.chunk;
        if (chunk == NULL) {
            break;
        }
        if (!ctx->shared->pcm_stream_active) {
            app_tasking_release_audio_pcm_chunk(chunk);
            break;
        }
        if (chunk->sample_count == 0U ||
            chunk->sample_count > AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES) {
            app_tasking_release_audio_pcm_chunk(chunk);
            break;
        }
        if (ctx->ensure_output_started() != ESP_OK) {
            app_tasking_release_audio_pcm_chunk(chunk);
            break;
        }
        {
            (*ctx->pcm_stream_rx_chunks)++;
            *ctx->pcm_stream_rx_samples += (uint32_t)chunk->sample_count;
            TickType_t now = xTaskGetTickCount();
            TickType_t diag_gap = audio_worker_ms_to_ticks_min1(1000U);
            if ((now - *ctx->pcm_stream_diag_last_log_tick) >= diag_gap) {
                *ctx->pcm_stream_diag_last_log_tick = now;
                uint32_t audio_ms = (uint32_t)(((uint64_t)(*ctx->pcm_stream_rx_samples) * 1000ULL) /
                                               (uint64_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ);
                ESP_LOGD(TAG,
                         "pcm stream rx diag chunks=%" PRIu32 " samples=%" PRIu32
                         " audio_ms=%" PRIu32 " bg=%u",
                         *ctx->pcm_stream_rx_chunks,
                         *ctx->pcm_stream_rx_samples,
                         audio_ms,
                         ctx->shared->bg.active ? 1U : 0U);
            }
            esp_err_t wr_err = audio_worker_write_mixed_output(ctx->shared,
                                                               chunk->samples,
                                                               (size_t)chunk->sample_count,
                                                               true);
            if (wr_err == ESP_ERR_TIMEOUT) {
                TickType_t gap = audio_worker_ms_to_ticks_min1(1000U);
                if ((now - ctx->shared->last_pcm_stream_timeout_log_tick) >= gap) {
                    ctx->shared->last_pcm_stream_timeout_log_tick = now;
                    ESP_LOGW(TAG, "PCM stream write timeout");
                }
            } else if (wr_err != ESP_OK) {
                ESP_LOGW(TAG, "PCM stream write failed: %s", esp_err_to_name(wr_err));
            } else {
                *ctx->pcm_stream_chunk_written_since_poll = true;
            }
            app_tasking_release_audio_pcm_chunk(chunk);
        }
        break;
    }

    case AUDIO_CMD_NONE:
    default:
        ESP_LOGW(TAG, "unknown command id=%d", (int)cmd->id);
        break;
    }
}
