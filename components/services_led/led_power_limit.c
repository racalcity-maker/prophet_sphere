#include "led_power_limit.h"

#include "sdkconfig.h"

static uint32_t clamp_scale_permille(uint32_t scale_permille)
{
    if (scale_permille > 1000U) {
        return 1000U;
    }
    if (scale_permille == 0U) {
        return 1U;
    }
    return scale_permille;
}

static uint32_t estimate_current_ma(const uint8_t *grb_data, size_t length_bytes)
{
    if (grb_data == NULL || length_bytes == 0U) {
        return 0U;
    }

    uint32_t channel_sum = 0U;
    for (size_t i = 0; i < length_bytes; ++i) {
        channel_sum += grb_data[i];
    }

    uint32_t dynamic_ma = (channel_sum * (uint32_t)CONFIG_ORB_LED_MAX_COLOR_CHANNEL_MA) / 255U;
    uint32_t pixels = (uint32_t)(length_bytes / 3U);
    uint32_t idle_ma = pixels * (uint32_t)CONFIG_ORB_LED_IDLE_CURRENT_MA;
    return dynamic_ma + idle_ma;
}

void led_power_limit_apply_grb(uint8_t *grb_data, size_t length_bytes, led_power_limit_result_t *result)
{
    led_power_limit_result_t local_result = { 0 };
    local_result.applied_scale_permille = 1000U;

    if (grb_data == NULL || length_bytes == 0U) {
        if (result != NULL) {
            *result = local_result;
        }
        return;
    }

#if CONFIG_ORB_LED_POWER_LIMIT_ENABLE
    uint32_t before_ma = estimate_current_ma(grb_data, length_bytes);
    local_result.estimated_current_ma_before = before_ma;
    uint32_t limit_ma = (uint32_t)CONFIG_ORB_LED_MAX_CURRENT_MA;

    if (before_ma > limit_ma && before_ma > 0U) {
        uint32_t scale_permille = (limit_ma * 1000U) / before_ma;
        scale_permille = clamp_scale_permille(scale_permille);

        for (size_t i = 0; i < length_bytes; ++i) {
            uint32_t scaled = ((uint32_t)grb_data[i] * scale_permille) / 1000U;
            if (scaled > 255U) {
                scaled = 255U;
            }
            grb_data[i] = (uint8_t)scaled;
        }

        local_result.limited = true;
        local_result.applied_scale_permille = (uint16_t)scale_permille;
    }

    local_result.estimated_current_ma_after = estimate_current_ma(grb_data, length_bytes);
#else
    local_result.estimated_current_ma_before = 0U;
    local_result.estimated_current_ma_after = 0U;
#endif

    if (result != NULL) {
        *result = local_result;
    }
}
