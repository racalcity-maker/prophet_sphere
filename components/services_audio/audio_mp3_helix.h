#ifndef AUDIO_MP3_HELIX_H
#define AUDIO_MP3_HELIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "mp3dec.h"

#ifndef CONFIG_ORB_AUDIO_MP3_INPUT_BUFFER_SIZE
#define CONFIG_ORB_AUDIO_MP3_INPUT_BUFFER_SIZE 8192
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    HMP3Decoder decoder;
    void *file;
    bool eof;
    bool frame_info_valid;
    uint8_t channels;
    uint32_t sample_rate_hz;
    size_t input_len;
    uint8_t input_buf[CONFIG_ORB_AUDIO_MP3_INPUT_BUFFER_SIZE];
    int16_t pcm_buf[2304];
    int16_t mono_buf[1152];
} audio_mp3_helix_ctx_t;

esp_err_t audio_mp3_helix_open(audio_mp3_helix_ctx_t *ctx, const char *path);
void audio_mp3_helix_close(audio_mp3_helix_ctx_t *ctx);
esp_err_t audio_mp3_helix_step(audio_mp3_helix_ctx_t *ctx,
                               const int16_t **out_samples,
                               size_t *out_sample_count,
                               bool *out_done);
uint32_t audio_mp3_helix_sample_rate_hz(const audio_mp3_helix_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
