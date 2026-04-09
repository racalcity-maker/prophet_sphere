#include "esp_err.h"
#include "esp_log.h"
#include "log_tags.h"
#include "orb_bootstrap.h"

static const char *TAG = LOG_TAG_APP_MAIN;

void app_main(void)
{
    esp_err_t err = orb_bootstrap_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "orb_bootstrap_start failed: %s", esp_err_to_name(err));
    }
}
