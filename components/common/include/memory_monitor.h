#ifndef MEMORY_MONITOR_H
#define MEMORY_MONITOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start periodic heap diagnostics logger.
 *
 * When enabled in Kconfig, prints free and largest free heap block for:
 * - Internal SRAM
 * - PSRAM
 * - Total 8-bit capable heap
 */
esp_err_t memory_monitor_start(void);

#ifdef __cplusplus
}
#endif

#endif
