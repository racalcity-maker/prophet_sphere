#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "storage_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thread-safe storage service for SD card access over SPI.
 * Provides mount lifecycle and simple file read helpers for higher-level modules
 * (for example future MP3 asset decode pipeline).
 */
esp_err_t storage_manager_init(void);
esp_err_t storage_manager_mount(void);
esp_err_t storage_manager_unmount(void);
bool storage_manager_is_mounted(void);
const char *storage_manager_mount_point(void);
esp_err_t storage_manager_get_status(storage_status_t *status);

/*
 * I/O guard for callers that perform multi-step filesystem operations
 * (for example readdir + stat scans). While held, unmount is serialized.
 * Always pair with storage_manager_unlock_for_io().
 */
esp_err_t storage_manager_lock_for_io(bool *out_mounted);
void storage_manager_unlock_for_io(void);

esp_err_t storage_manager_read_file_abs(const char *abs_path, uint8_t *buffer, size_t buffer_len, size_t *out_read);
esp_err_t storage_manager_read_file_rel(const char *relative_path, uint8_t *buffer, size_t buffer_len, size_t *out_read);

#ifdef __cplusplus
}
#endif

#endif
