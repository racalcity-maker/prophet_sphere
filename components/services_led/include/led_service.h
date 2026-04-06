#ifndef LED_SERVICE_H
#define LED_SERVICE_H

#include "esp_err.h"
#include "led_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Queue-based and thread-safe frontend.
 * led_task is the only owner of LED rendering/hardware state.
 */
esp_err_t led_service_init(void);
esp_err_t led_service_start_task(void);
esp_err_t led_service_stop_task(void);
esp_err_t led_service_play_scene(led_scene_id_t scene_id, uint32_t duration_ms, uint32_t timeout_ms);
esp_err_t led_service_stop(uint32_t timeout_ms);
esp_err_t led_service_set_brightness(led_brightness_t brightness, uint32_t timeout_ms);
esp_err_t led_service_set_effect_params(uint8_t speed, uint8_t intensity, uint8_t scale, uint32_t timeout_ms);
esp_err_t led_service_set_effect_palette(uint8_t mode,
                                         uint8_t color1_r,
                                         uint8_t color1_g,
                                         uint8_t color1_b,
                                         uint8_t color2_r,
                                         uint8_t color2_g,
                                         uint8_t color2_b,
                                         uint8_t color3_r,
                                         uint8_t color3_g,
                                         uint8_t color3_b,
                                         uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
