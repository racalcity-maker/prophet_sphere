#include "touch_service.h"

#include "driver/touch_sensor.h"
#include "esp_bit_defs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "log_tags.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "touch_task.h"

static const char *TAG = LOG_TAG_TOUCH;

static bool s_real_touch_enabled;
static touch_hw_channel_t s_zone_channels[TOUCH_ZONE_COUNT];
static const touch_hw_channel_t s_required_channels[TOUCH_ZONE_COUNT] = { 1, 2, 3, 4 };

static esp_err_t validate_zone_channels(void)
{
    for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        touch_hw_channel_t channel = s_zone_channels[i];
        if (channel <= 0 || channel >= SOC_TOUCH_SENSOR_NUM) {
            ESP_LOGE(TAG, "invalid touch channel for zone %d: %d (valid range: 1..%d)", i, channel, SOC_TOUCH_SENSOR_NUM - 1);
            return ESP_ERR_INVALID_ARG;
        }
        if (channel != s_required_channels[i]) {
            ESP_LOGE(TAG, "zone %d must use channel %d (configured %d)", i, s_required_channels[i], channel);
            return ESP_ERR_INVALID_ARG;
        }
    }

    for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        for (int j = i + 1; j < TOUCH_ZONE_COUNT; ++j) {
            if (s_zone_channels[i] == s_zone_channels[j]) {
                ESP_LOGE(TAG, "duplicate touch channel configuration: zone %d and zone %d both use channel %d", i, j, s_zone_channels[i]);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t init_touch_peripheral(void)
{
    uint16_t channel_mask = 0;

    ESP_RETURN_ON_ERROR(touch_pad_init(), TAG, "touch_pad_init failed");
    ESP_RETURN_ON_ERROR(touch_pad_fsm_stop(), TAG, "touch_pad_fsm_stop failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER), TAG, "touch_pad_set_fsm_mode failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_voltage(TOUCH_PAD_HIGH_VOLTAGE_THRESHOLD,
                                              TOUCH_PAD_LOW_VOLTAGE_THRESHOLD,
                                              TOUCH_PAD_ATTEN_VOLTAGE_THRESHOLD),
                        TAG,
                        "touch_pad_set_voltage failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_idle_channel_connect(TOUCH_PAD_CONN_GND),
                        TAG,
                        "touch_pad_set_idle_channel_connect failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_charge_discharge_times(TOUCH_PAD_MEASURE_CYCLE_DEFAULT),
                        TAG,
                        "touch_pad_set_charge_discharge_times failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_measurement_interval(TOUCH_PAD_SLEEP_CYCLE_DEFAULT),
                        TAG,
                        "touch_pad_set_measurement_interval failed");

    for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        touch_pad_t pad = (touch_pad_t)s_zone_channels[i];
        ESP_RETURN_ON_ERROR(touch_pad_config(pad), TAG, "touch_pad_config failed for zone %d", i);
        channel_mask |= (uint16_t)BIT(pad);
    }

    ESP_RETURN_ON_ERROR(touch_pad_set_channel_mask(channel_mask), TAG, "touch_pad_set_channel_mask failed");
    ESP_RETURN_ON_ERROR(touch_pad_fsm_start(), TAG, "touch_pad_fsm_start failed");
    return ESP_OK;
}

esp_err_t touch_service_init(void)
{
    s_zone_channels[TOUCH_ZONE_0] = (touch_hw_channel_t)CONFIG_ORB_TOUCH_ZONE0_CHANNEL;
    s_zone_channels[TOUCH_ZONE_1] = (touch_hw_channel_t)CONFIG_ORB_TOUCH_ZONE1_CHANNEL;
    s_zone_channels[TOUCH_ZONE_2] = (touch_hw_channel_t)CONFIG_ORB_TOUCH_ZONE2_CHANNEL;
    s_zone_channels[TOUCH_ZONE_3] = (touch_hw_channel_t)CONFIG_ORB_TOUCH_ZONE3_CHANNEL;

#if CONFIG_ORB_TOUCH_ENABLE_REAL
    ESP_RETURN_ON_ERROR(validate_zone_channels(), TAG, "touch channel validation failed");
    ESP_RETURN_ON_ERROR(init_touch_peripheral(), TAG, "touch peripheral init failed");
    s_real_touch_enabled = true;
    ESP_LOGI(TAG,
             "real touch enabled on fixed channels: z0=%d(GPIO1) z1=%d(GPIO2) z2=%d(GPIO3) z3=%d(GPIO4)",
             s_zone_channels[TOUCH_ZONE_0],
             s_zone_channels[TOUCH_ZONE_1],
             s_zone_channels[TOUCH_ZONE_2],
             s_zone_channels[TOUCH_ZONE_3]);
#else
    s_real_touch_enabled = false;
    ESP_LOGW(TAG, "real touch disabled by config; touch events will not be produced");
#endif
    return ESP_OK;
}

esp_err_t touch_service_start_task(void)
{
    if (CONFIG_ORB_TOUCH_ENABLE_REAL && !s_real_touch_enabled) {
        ESP_RETURN_ON_ERROR(touch_service_init(), TAG, "touch re-init failed");
    }
    return touch_task_start();
}

esp_err_t touch_service_stop_task(void)
{
    esp_err_t err = touch_task_stop();
    if (!s_real_touch_enabled) {
        return err;
    }

    (void)touch_pad_fsm_stop();
    (void)touch_pad_deinit();
    s_real_touch_enabled = false;
    ESP_LOGI(TAG, "touch peripheral stopped");
    return err;
}

bool touch_service_real_touch_enabled(void)
{
    return s_real_touch_enabled;
}

esp_err_t touch_service_get_zone_channels(touch_hw_channel_t channels[TOUCH_ZONE_COUNT])
{
    if (channels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < TOUCH_ZONE_COUNT; ++i) {
        channels[i] = s_zone_channels[i];
    }
    return ESP_OK;
}

esp_err_t touch_service_get_runtime_config(touch_runtime_config_t *out_config)
{
    return touch_task_get_runtime_config(out_config);
}

esp_err_t touch_service_apply_runtime_config(const touch_runtime_config_t *config,
                                             bool recalibrate_now,
                                             touch_reconfig_scope_t *out_scope)
{
    if (out_scope != NULL) {
        *out_scope = TOUCH_RECONFIG_SCOPE_HOT_APPLY;
    }
    esp_err_t err = touch_task_apply_runtime_config(config, recalibrate_now);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "touch runtime config applied scope=hot recalibrate=%d", recalibrate_now ? 1 : 0);
    }
    return err;
}

esp_err_t touch_service_request_recalibration(void)
{
    touch_runtime_config_t cfg = { 0 };
    esp_err_t err = touch_task_get_runtime_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return touch_task_apply_runtime_config(&cfg, true);
}
