#include "prompt_engine.h"

#include <stdio.h>
#include "esp_log.h"
#include "log_tags.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_PROMPT;

esp_err_t prompt_engine_init(void)
{
    ESP_LOGI(TAG, "prompt engine initialized max_len=%d", CONFIG_ORB_AI_PROMPT_MAX_LEN);
    return ESP_OK;
}

esp_err_t prompt_engine_compose(const char *input, char *out_buf, size_t out_len)
{
    if (out_buf == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (input == NULL) {
        input = "";
    }

    (void)snprintf(out_buf, out_len, "OrbContext:%s", input);
    return ESP_OK;
}
