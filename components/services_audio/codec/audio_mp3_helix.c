#include "audio_mp3_helix.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "log_tags.h"

#ifndef CONFIG_ORB_AUDIO_MP3_READ_CHUNK_SIZE
#define CONFIG_ORB_AUDIO_MP3_READ_CHUNK_SIZE 1024
#endif

static const char *TAG = LOG_TAG_AUDIO;

static size_t min_size(size_t a, size_t b)
{
    return (a < b) ? a : b;
}

static FILE *ctx_file(audio_mp3_helix_ctx_t *ctx)
{
    return (FILE *)ctx->file;
}

static void shift_input(audio_mp3_helix_ctx_t *ctx, size_t drop_bytes)
{
    if (drop_bytes >= ctx->input_len) {
        ctx->input_len = 0;
        return;
    }

    size_t remain = ctx->input_len - drop_bytes;
    memmove(ctx->input_buf, ctx->input_buf + drop_bytes, remain);
    ctx->input_len = remain;
}

static esp_err_t refill_input(audio_mp3_helix_ctx_t *ctx)
{
    if (ctx->eof) {
        return ESP_OK;
    }

    if (ctx->input_len >= sizeof(ctx->input_buf)) {
        return ESP_OK;
    }

    size_t free_space = sizeof(ctx->input_buf) - ctx->input_len;
    size_t read_req = min_size(free_space, (size_t)CONFIG_ORB_AUDIO_MP3_READ_CHUNK_SIZE);
    if (read_req == 0U) {
        return ESP_OK;
    }

    size_t n = fread(ctx->input_buf + ctx->input_len, 1, read_req, ctx_file(ctx));
    if (n > 0) {
        ctx->input_len += n;
        return ESP_OK;
    }
    if (feof(ctx_file(ctx))) {
        ctx->eof = true;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t audio_mp3_helix_open(audio_mp3_helix_ctx_t *ctx, const char *path)
{
    if (ctx == NULL || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    HMP3Decoder dec = MP3InitDecoder();
    if (dec == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    ctx->file = (void *)f;
    ctx->decoder = dec;
    ctx->eof = false;
    ctx->frame_info_valid = false;
    ctx->channels = 0U;
    ctx->sample_rate_hz = 0U;
    ctx->input_len = 0;

    ESP_LOGD(TAG, "helix open: %s", path);
    return ESP_OK;
}

void audio_mp3_helix_close(audio_mp3_helix_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->decoder != NULL) {
        MP3FreeDecoder(ctx->decoder);
        ctx->decoder = NULL;
    }
    if (ctx->file != NULL) {
        fclose(ctx_file(ctx));
        ctx->file = NULL;
    }
    ctx->eof = false;
    ctx->frame_info_valid = false;
    ctx->channels = 0U;
    ctx->sample_rate_hz = 0U;
    ctx->input_len = 0;
}

esp_err_t audio_mp3_helix_step(audio_mp3_helix_ctx_t *ctx,
                               const int16_t **out_samples,
                               size_t *out_sample_count,
                               bool *out_done)
{
    if (ctx == NULL || out_samples == NULL || out_sample_count == NULL || out_done == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_samples = NULL;
    *out_sample_count = 0;
    *out_done = false;

    if (ctx->decoder == NULL || ctx->file == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(refill_input(ctx), TAG, "mp3 refill failed");

    if (ctx->input_len == 0U && ctx->eof) {
        *out_done = true;
        return ESP_OK;
    }

    if (ctx->input_len < 4U) {
        return ESP_ERR_NOT_FINISHED;
    }

    int sync = MP3FindSyncWord(ctx->input_buf, (int)ctx->input_len);
    if (sync < 0) {
        if (ctx->eof) {
            ctx->input_len = 0;
            *out_done = true;
            return ESP_OK;
        }
        size_t tail = min_size(ctx->input_len, (size_t)3U);
        if (tail > 0U) {
            memmove(ctx->input_buf, ctx->input_buf + (ctx->input_len - tail), tail);
        }
        ctx->input_len = tail;
        return ESP_ERR_NOT_FINISHED;
    }
    if (sync > 0) {
        shift_input(ctx, (size_t)sync);
        if (ctx->input_len < 4U) {
            return ESP_ERR_NOT_FINISHED;
        }
    }

    unsigned char *in_ptr = ctx->input_buf;
    int bytes_left = (int)ctx->input_len;
    int rc = MP3Decode(ctx->decoder, &in_ptr, &bytes_left, ctx->pcm_buf, 0);

    size_t consumed = (size_t)(in_ptr - ctx->input_buf);
    size_t remain = (bytes_left > 0) ? (size_t)bytes_left : 0U;
    if (consumed > 0U || remain != ctx->input_len) {
        if (remain > 0U) {
            memmove(ctx->input_buf, in_ptr, remain);
        }
        ctx->input_len = remain;
    }

    if (rc == ERR_MP3_NONE) {
        MP3FrameInfo frame = { 0 };
        MP3GetLastFrameInfo(ctx->decoder, &frame);
        if (frame.outputSamps <= 0 || frame.nChans <= 0) {
            return ESP_ERR_NOT_FINISHED;
        }
        ctx->frame_info_valid = true;
        ctx->channels = (uint8_t)frame.nChans;
        ctx->sample_rate_hz = (frame.samprate > 0) ? (uint32_t)frame.samprate : 0U;

        if (frame.nChans == 1) {
            *out_samples = ctx->pcm_buf;
            *out_sample_count = (size_t)frame.outputSamps;
            return ESP_OK;
        }

        size_t stereo_samps = (size_t)frame.outputSamps;
        size_t mono_frames = stereo_samps / 2U;
        if (mono_frames > (sizeof(ctx->mono_buf) / sizeof(ctx->mono_buf[0]))) {
            mono_frames = sizeof(ctx->mono_buf) / sizeof(ctx->mono_buf[0]);
        }

        for (size_t i = 0; i < mono_frames; ++i) {
            int32_t l = (int32_t)ctx->pcm_buf[i * 2U];
            int32_t r = (int32_t)ctx->pcm_buf[(i * 2U) + 1U];
            ctx->mono_buf[i] = (int16_t)((l + r) / 2);
        }

        *out_samples = ctx->mono_buf;
        *out_sample_count = mono_frames;
        return ESP_OK;
    }

    if (rc == ERR_MP3_INDATA_UNDERFLOW || rc == ERR_MP3_MAINDATA_UNDERFLOW) {
        if (ctx->eof) {
            if (ctx->input_len == 0U) {
                *out_done = true;
                return ESP_OK;
            }
            shift_input(ctx, 1U);
            if (ctx->input_len == 0U) {
                *out_done = true;
                return ESP_OK;
            }
        }
        return ESP_ERR_NOT_FINISHED;
    }

    if (ctx->eof && ctx->input_len == 0U) {
        *out_done = true;
        return ESP_OK;
    }

    /* Attempt soft resync on frame corruption. */
    if (ctx->input_len > 1U) {
        shift_input(ctx, 1U);
        return ESP_ERR_NOT_FINISHED;
    }

    ESP_LOGW(TAG, "helix decode error rc=%d", rc);
    return ESP_FAIL;
}

uint32_t audio_mp3_helix_sample_rate_hz(const audio_mp3_helix_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->frame_info_valid) {
        return 0U;
    }
    return ctx->sample_rate_hz;
}
