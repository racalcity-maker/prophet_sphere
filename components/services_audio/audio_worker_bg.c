#include "audio_worker_bg.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "app_events.h"
#include "audio_worker_internal.h"
#include "audio_types.h"
#include "audio_worker_reactive.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_AUDIO;

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

void audio_worker_bg_reset_state(void)
{
    memset(&s_bg, 0, sizeof(s_bg));
}

void audio_worker_bg_close(void)
{
    if (s_bg.file != NULL) {
        (void)fclose(s_bg.file);
        s_bg.file = NULL;
    }
    audio_worker_bg_reset_state();
}

static bool bg_parse_wav_header(FILE *file,
                                uint32_t *out_data_offset,
                                uint32_t *out_data_size,
                                uint32_t *out_sample_rate)
{
    uint8_t riff_header[12] = { 0 };
    if (fread(riff_header, 1, sizeof(riff_header), file) != sizeof(riff_header)) {
        return false;
    }
    if (memcmp(&riff_header[0], "RIFF", 4) != 0 || memcmp(&riff_header[8], "WAVE", 4) != 0) {
        return false;
    }

    bool fmt_found = false;
    bool data_found = false;
    uint16_t audio_format = 0U;
    uint16_t channels = 0U;
    uint16_t bits_per_sample = 0U;
    uint32_t sample_rate = 0U;
    uint32_t data_offset = 0U;
    uint32_t data_size = 0U;

    while (!data_found) {
        uint8_t chunk_header[8] = { 0 };
        if (fread(chunk_header, 1, sizeof(chunk_header), file) != sizeof(chunk_header)) {
            break;
        }
        uint32_t chunk_size = read_le32(&chunk_header[4]);

        if (memcmp(&chunk_header[0], "fmt ", 4) == 0) {
            uint8_t fmt[40] = { 0 };
            uint32_t to_read = (chunk_size < sizeof(fmt)) ? chunk_size : (uint32_t)sizeof(fmt);
            if (fread(fmt, 1, to_read, file) != to_read) {
                return false;
            }
            if (chunk_size > to_read) {
                if (fseek(file, (long)(chunk_size - to_read), SEEK_CUR) != 0) {
                    return false;
                }
            }
            if (chunk_size < 16U) {
                return false;
            }
            audio_format = read_le16(&fmt[0]);
            channels = read_le16(&fmt[2]);
            sample_rate = read_le32(&fmt[4]);
            bits_per_sample = read_le16(&fmt[14]);
            fmt_found = true;
        } else if (memcmp(&chunk_header[0], "data", 4) == 0) {
            long pos = ftell(file);
            if (pos < 0L) {
                return false;
            }
            data_offset = (uint32_t)pos;
            data_size = chunk_size;
            if (fseek(file, (long)chunk_size, SEEK_CUR) != 0) {
                return false;
            }
            data_found = true;
        } else {
            if (fseek(file, (long)chunk_size, SEEK_CUR) != 0) {
                return false;
            }
        }

        if ((chunk_size & 1U) != 0U) {
            if (fseek(file, 1L, SEEK_CUR) != 0) {
                return false;
            }
        }
    }

    if (!fmt_found || !data_found) {
        return false;
    }
    if (audio_format != 1U || channels != 1U || bits_per_sample != 16U) {
        ESP_LOGW(TAG,
                 "background wav format unsupported (fmt=%u ch=%u bits=%u)",
                 (unsigned)audio_format,
                 (unsigned)channels,
                 (unsigned)bits_per_sample);
        return false;
    }
    if (sample_rate != (uint32_t)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ) {
        ESP_LOGW(TAG,
                 "background wav sample rate mismatch (%" PRIu32 " != %u)",
                 sample_rate,
                 (unsigned)CONFIG_ORB_AUDIO_I2S_SAMPLE_RATE_HZ);
        return false;
    }
    if (data_size < 2U) {
        return false;
    }

    *out_data_offset = data_offset;
    *out_data_size = data_size;
    *out_sample_rate = sample_rate;
    return true;
}

esp_err_t audio_worker_bg_start(uint32_t fade_in_ms, uint16_t gain_permille)
{
    if (gain_permille == 0U) {
        gain_permille = (uint16_t)CONFIG_ORB_AUDIO_PROPHECY_BG_GAIN_PERMILLE;
    }
    if (gain_permille > 1000U) {
        gain_permille = 1000U;
    }

    FILE *f = fopen(CONFIG_ORB_AUDIO_PROPHECY_BG_WAV_PATH, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "background wav open failed path=%s errno=%d", CONFIG_ORB_AUDIO_PROPHECY_BG_WAV_PATH, errno);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t data_offset = 0U;
    uint32_t data_size = 0U;
    uint32_t sample_rate = 0U;
    if (!bg_parse_wav_header(f, &data_offset, &data_size, &sample_rate)) {
        (void)fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (fseek(f, (long)data_offset, SEEK_SET) != 0) {
        (void)fclose(f);
        return ESP_FAIL;
    }

    audio_worker_bg_close();
    s_bg.file = f;
    s_bg.data_offset = data_offset;
    s_bg.data_size_bytes = data_size;
    s_bg.data_pos_bytes = 0U;
    s_bg.sample_rate_hz = sample_rate;
    s_bg.active = true;
    s_bg.fade_post_done_event = false;
    s_bg.fade_start_gain_permille = 0U;
    s_bg.fade_target_gain_permille = gain_permille;
    s_bg.gain_permille = (fade_in_ms > 0U) ? 0U : gain_permille;
    s_bg.fade_active = (fade_in_ms > 0U);
    s_bg.fade_total_samples = audio_worker_ms_to_samples(fade_in_ms);
    s_bg.fade_done_samples = 0U;

    ESP_LOGI(TAG,
             "background start path=%s fade=%" PRIu32 "ms gain=%u",
             CONFIG_ORB_AUDIO_PROPHECY_BG_WAV_PATH,
             fade_in_ms,
             (unsigned)gain_permille);
    return ESP_OK;
}

void audio_worker_bg_begin_fade(uint16_t target_gain_permille, uint32_t fade_ms, bool post_done_event)
{
    if (!s_bg.active) {
        return;
    }
    s_bg.fade_start_gain_permille = s_bg.gain_permille;
    s_bg.fade_target_gain_permille = target_gain_permille;
    s_bg.fade_total_samples = audio_worker_ms_to_samples(fade_ms);
    s_bg.fade_done_samples = 0U;
    s_bg.fade_active = (s_bg.fade_total_samples > 0U);
    s_bg.fade_post_done_event = post_done_event;

    if (!s_bg.fade_active) {
        s_bg.gain_permille = target_gain_permille;
    }
}

size_t audio_worker_bg_read_samples(int16_t *out_samples, size_t sample_count)
{
    if (!s_bg.active || s_bg.file == NULL || out_samples == NULL || sample_count == 0U) {
        return 0U;
    }

    size_t total = 0U;
    while (total < sample_count) {
        if (s_bg.data_pos_bytes >= s_bg.data_size_bytes) {
            if (fseek(s_bg.file, (long)s_bg.data_offset, SEEK_SET) != 0) {
                ESP_LOGW(TAG, "background rewind failed");
                audio_worker_bg_close();
                break;
            }
            s_bg.data_pos_bytes = 0U;
        }

        uint32_t bytes_left = s_bg.data_size_bytes - s_bg.data_pos_bytes;
        size_t samples_left = (size_t)(bytes_left / 2U);
        if (samples_left == 0U) {
            if (fseek(s_bg.file, (long)s_bg.data_offset, SEEK_SET) != 0) {
                ESP_LOGW(TAG, "background rewind failed");
                audio_worker_bg_close();
                break;
            }
            s_bg.data_pos_bytes = 0U;
            continue;
        }

        size_t need = sample_count - total;
        size_t chunk = (need < samples_left) ? need : samples_left;
        size_t got = fread(&out_samples[total], sizeof(int16_t), chunk, s_bg.file);
        if (got == 0U) {
            ESP_LOGW(TAG, "background read failed");
            audio_worker_bg_close();
            break;
        }

        total += got;
        s_bg.data_pos_bytes += (uint32_t)(got * sizeof(int16_t));
    }
    return total;
}

void audio_worker_bg_update_fade(size_t consumed_samples)
{
    if (!s_bg.active || !s_bg.fade_active || consumed_samples == 0U) {
        return;
    }

    uint64_t done = (uint64_t)s_bg.fade_done_samples + (uint64_t)consumed_samples;
    if (done >= (uint64_t)s_bg.fade_total_samples) {
        s_bg.fade_done_samples = s_bg.fade_total_samples;
    } else {
        s_bg.fade_done_samples = (uint32_t)done;
    }

    if (s_bg.fade_total_samples == 0U || s_bg.fade_done_samples >= s_bg.fade_total_samples) {
        s_bg.gain_permille = s_bg.fade_target_gain_permille;
        s_bg.fade_active = false;
    } else {
        int32_t delta = (int32_t)s_bg.fade_target_gain_permille - (int32_t)s_bg.fade_start_gain_permille;
        int32_t numer = delta * (int32_t)s_bg.fade_done_samples;
        int32_t step = numer / (int32_t)s_bg.fade_total_samples;
        int32_t gain = (int32_t)s_bg.fade_start_gain_permille + step;
        if (gain < 0) {
            gain = 0;
        } else if (gain > 1000) {
            gain = 1000;
        }
        s_bg.gain_permille = (uint16_t)gain;
    }

    if (!s_bg.fade_active && s_bg.gain_permille == 0U) {
        bool post_done = s_bg.fade_post_done_event;
        audio_worker_bg_close();
        if (post_done) {
            (void)audio_worker_post_audio_done(0U, (int32_t)APP_AUDIO_DONE_CODE_BG_FADE_COMPLETE);
        }
    }
}
