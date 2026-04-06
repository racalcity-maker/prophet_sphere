#include "audio_dac_backend.h"

#include "esp_err.h"
#include "esp_log.h"
#include "log_tags.h"

#if CONFIG_ORB_AUDIO_DAC_PCM5102A
static const char *TAG = LOG_TAG_AUDIO_I2S;

static esp_err_t pcm5102a_init(void)
{
    ESP_LOGI(TAG, "DAC profile: PCM5102A");
    return ESP_OK;
}

static esp_err_t pcm5102a_start(void)
{
    return ESP_OK;
}

static esp_err_t pcm5102a_stop(void)
{
    return ESP_OK;
}

const audio_dac_backend_t g_audio_dac_backend = {
    .name = "pcm5102a",
    .init = pcm5102a_init,
    .start = pcm5102a_start,
    .stop = pcm5102a_stop,
};
#endif
