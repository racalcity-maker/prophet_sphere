#include "storage_manager.h"

#include <stdio.h>
#include <string.h>
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_tags.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

static const char *TAG = LOG_TAG_STORAGE;

#ifndef CONFIG_ORB_ENABLE_STORAGE_SD
#define CONFIG_ORB_ENABLE_STORAGE_SD 0
#endif
#ifndef CONFIG_ORB_STORAGE_MOUNT_POINT
#define CONFIG_ORB_STORAGE_MOUNT_POINT "/sdcard"
#endif
#ifndef CONFIG_ORB_STORAGE_MAX_OPEN_FILES
#define CONFIG_ORB_STORAGE_MAX_OPEN_FILES 5
#endif
#ifndef CONFIG_ORB_STORAGE_FORMAT_IF_MOUNT_FAILED
#define CONFIG_ORB_STORAGE_FORMAT_IF_MOUNT_FAILED 0
#endif
#ifndef CONFIG_ORB_STORAGE_SPI_HOST_SPI3
#define CONFIG_ORB_STORAGE_SPI_HOST_SPI3 0
#endif
#ifndef CONFIG_ORB_STORAGE_SPI_MOSI_GPIO
#define CONFIG_ORB_STORAGE_SPI_MOSI_GPIO 11
#endif
#ifndef CONFIG_ORB_STORAGE_SPI_MISO_GPIO
#define CONFIG_ORB_STORAGE_SPI_MISO_GPIO 13
#endif
#ifndef CONFIG_ORB_STORAGE_SPI_SCLK_GPIO
#define CONFIG_ORB_STORAGE_SPI_SCLK_GPIO 12
#endif
#ifndef CONFIG_ORB_STORAGE_SPI_CS_GPIO
#define CONFIG_ORB_STORAGE_SPI_CS_GPIO 10
#endif
#ifndef CONFIG_ORB_STORAGE_SPI_CLOCK_KHZ
#define CONFIG_ORB_STORAGE_SPI_CLOCK_KHZ 10000
#endif
#ifndef CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS
#define CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS 50
#endif

static SemaphoreHandle_t s_storage_mutex;
static sdmmc_card_t *s_card;
static bool s_mounted;
static bool s_bus_initialized;
static int s_spi_host_id;

static TickType_t lock_timeout_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    return (ticks > 0) ? ticks : 1;
}

static esp_err_t storage_lock(void)
{
    if (s_storage_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_storage_mutex, lock_timeout_ticks()) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void storage_unlock(void)
{
    if (s_storage_mutex != NULL) {
        xSemaphoreGive(s_storage_mutex);
    }
}

static int select_spi_host(void)
{
#if CONFIG_ORB_STORAGE_SPI_HOST_SPI3
    return SPI3_HOST;
#else
    return SPI2_HOST;
#endif
}

static esp_err_t build_absolute_path(const char *relative_path, char *out_path, size_t out_path_len)
{
    if (relative_path == NULL || out_path == NULL || out_path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (relative_path[0] == '/') {
        if (strncmp(relative_path, CONFIG_ORB_STORAGE_MOUNT_POINT, strlen(CONFIG_ORB_STORAGE_MOUNT_POINT)) == 0) {
            int n = snprintf(out_path, out_path_len, "%s", relative_path);
            return (n > 0 && (size_t)n < out_path_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        int n = snprintf(out_path, out_path_len, "%s%s", CONFIG_ORB_STORAGE_MOUNT_POINT, relative_path);
        return (n > 0 && (size_t)n < out_path_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    int n = snprintf(out_path, out_path_len, "%s/%s", CONFIG_ORB_STORAGE_MOUNT_POINT, relative_path);
    return (n > 0 && (size_t)n < out_path_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t storage_manager_init(void)
{
#if !CONFIG_ORB_ENABLE_STORAGE_SD
    ESP_LOGW(TAG, "SD storage disabled by config");
    return ESP_OK;
#else
    if (s_storage_mutex != NULL) {
        return ESP_OK;
    }

    s_storage_mutex = xSemaphoreCreateMutex();
    if (s_storage_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_spi_host_id = select_spi_host();
    ESP_LOGI(TAG,
             "storage init host=%d mnt=%s cs=%d sclk=%d miso=%d mosi=%d",
             s_spi_host_id,
             CONFIG_ORB_STORAGE_MOUNT_POINT,
             CONFIG_ORB_STORAGE_SPI_CS_GPIO,
             CONFIG_ORB_STORAGE_SPI_SCLK_GPIO,
             CONFIG_ORB_STORAGE_SPI_MISO_GPIO,
             CONFIG_ORB_STORAGE_SPI_MOSI_GPIO);
    return ESP_OK;
#endif
}

esp_err_t storage_manager_mount(void)
{
#if !CONFIG_ORB_ENABLE_STORAGE_SD
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_RETURN_ON_ERROR(storage_manager_init(), TAG, "storage init failed");
    ESP_RETURN_ON_ERROR(storage_lock(), TAG, "storage lock failed");
    if (s_mounted) {
        storage_unlock();
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_ORB_STORAGE_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_ORB_STORAGE_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_ORB_STORAGE_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(s_spi_host_id, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err == ESP_OK) {
        s_bus_initialized = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        storage_unlock();
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_spi_host_id;
    host.max_freq_khz = CONFIG_ORB_STORAGE_SPI_CLOCK_KHZ;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = s_spi_host_id;
    slot_cfg.gpio_cs = CONFIG_ORB_STORAGE_SPI_CS_GPIO;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = CONFIG_ORB_STORAGE_FORMAT_IF_MOUNT_FAILED,
        .max_files = CONFIG_ORB_STORAGE_MAX_OPEN_FILES,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(CONFIG_ORB_STORAGE_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (s_bus_initialized) {
            (void)spi_bus_free(s_spi_host_id);
            s_bus_initialized = false;
        }
        storage_unlock();
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s", CONFIG_ORB_STORAGE_MOUNT_POINT);
    storage_unlock();
    return ESP_OK;
#endif
}

esp_err_t storage_manager_unmount(void)
{
#if !CONFIG_ORB_ENABLE_STORAGE_SD
    return ESP_OK;
#else
    ESP_RETURN_ON_ERROR(storage_manager_init(), TAG, "storage init failed");
    ESP_RETURN_ON_ERROR(storage_lock(), TAG, "storage lock failed");
    if (!s_mounted) {
        storage_unlock();
        return ESP_OK;
    }

    esp_vfs_fat_sdcard_unmount(CONFIG_ORB_STORAGE_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;

    if (s_bus_initialized) {
        (void)spi_bus_free(s_spi_host_id);
        s_bus_initialized = false;
    }

    ESP_LOGI(TAG, "SD unmounted");
    storage_unlock();
    return ESP_OK;
#endif
}

bool storage_manager_is_mounted(void)
{
#if !CONFIG_ORB_ENABLE_STORAGE_SD
    return false;
#else
    if (storage_manager_init() != ESP_OK) {
        return false;
    }
    if (storage_lock() != ESP_OK) {
        return false;
    }
    bool mounted = s_mounted;
    storage_unlock();
    return mounted;
#endif
}

const char *storage_manager_mount_point(void)
{
    return CONFIG_ORB_STORAGE_MOUNT_POINT;
}

esp_err_t storage_manager_get_status(storage_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    (void)snprintf(status->mount_point, sizeof(status->mount_point), "%s", CONFIG_ORB_STORAGE_MOUNT_POINT);
    status->mounted = storage_manager_is_mounted();
    if (!status->mounted) {
        return ESP_OK;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(CONFIG_ORB_STORAGE_MOUNT_POINT, &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        return err;
    }

    status->total_bytes = total_bytes;
    status->free_bytes = free_bytes;
    return ESP_OK;
}

esp_err_t storage_manager_lock_for_io(bool *out_mounted)
{
#if !CONFIG_ORB_ENABLE_STORAGE_SD
    if (out_mounted != NULL) {
        *out_mounted = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_RETURN_ON_ERROR(storage_manager_init(), TAG, "storage init failed");
    ESP_RETURN_ON_ERROR(storage_lock(), TAG, "storage lock failed");
    if (out_mounted != NULL) {
        *out_mounted = s_mounted;
    }
    return ESP_OK;
#endif
}

void storage_manager_unlock_for_io(void)
{
#if CONFIG_ORB_ENABLE_STORAGE_SD
    storage_unlock();
#endif
}

esp_err_t storage_manager_read_file_abs(const char *abs_path, uint8_t *buffer, size_t buffer_len, size_t *out_read)
{
    if (abs_path == NULL || buffer == NULL || buffer_len == 0 || out_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_read = 0;

    bool mounted = false;
    esp_err_t lock_err = storage_manager_lock_for_io(&mounted);
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    if (!mounted) {
        storage_manager_unlock_for_io();
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(abs_path, "rb");
    if (f == NULL) {
        storage_manager_unlock_for_io();
        return ESP_ERR_NOT_FOUND;
    }

    size_t n = fread(buffer, 1, buffer_len, f);
    if (ferror(f)) {
        fclose(f);
        storage_manager_unlock_for_io();
        return ESP_FAIL;
    }
    fclose(f);
    storage_manager_unlock_for_io();

    *out_read = n;
    return ESP_OK;
}

esp_err_t storage_manager_read_file_rel(const char *relative_path, uint8_t *buffer, size_t buffer_len, size_t *out_read)
{
    char abs_path[160] = { 0 };
    ESP_RETURN_ON_ERROR(build_absolute_path(relative_path, abs_path, sizeof(abs_path)), TAG, "path too long");
    return storage_manager_read_file_abs(abs_path, buffer, buffer_len, out_read);
}
