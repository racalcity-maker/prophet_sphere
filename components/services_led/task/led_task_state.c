#include "led_task_internal.h"

#include "freertos/task.h"

TaskHandle_t s_led_task_handle = NULL;
volatile bool s_stop_requested = false;
uint32_t s_last_limit_log_ms = 0U;

led_runtime_t s_runtime;
uint8_t s_framebuffer[LED_FRAMEBUFFER_BYTES];

