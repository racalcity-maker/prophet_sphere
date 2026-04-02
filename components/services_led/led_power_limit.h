#ifndef LED_POWER_LIMIT_H
#define LED_POWER_LIMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool limited;
    uint32_t estimated_current_ma_before;
    uint32_t estimated_current_ma_after;
    uint16_t applied_scale_permille;
} led_power_limit_result_t;

void led_power_limit_apply_grb(uint8_t *grb_data, size_t length_bytes, led_power_limit_result_t *result);

#endif
