#include "bsp_board.h"

#include "bsp_pins.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_BSP;

esp_err_t bsp_board_init(void)
{
    ESP_LOGI(TAG,
             "board=%s led=%d status=%d led_data=%d amp_en=%d mode_btn=%d",
             CONFIG_ORB_DEVICE_NAME,
             BSP_BOARD_LED_GPIO,
             BSP_STATUS_GPIO,
             BSP_LED_DATA_GPIO,
             BSP_AMP_ENABLE_GPIO,
             BSP_MODE_BUTTON_GPIO);
    return ESP_OK;
}

const char *bsp_board_name(void)
{
    return CONFIG_ORB_DEVICE_NAME;
}
