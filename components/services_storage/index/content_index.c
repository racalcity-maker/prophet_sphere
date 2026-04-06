#include "content_index.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_random.h"
#include "storage_manager.h"

#define CONTENT_INDEX_PATH_MAX 384U

static bool has_mp3_extension(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t len = strlen(name);
    if (len < 4U) {
        return false;
    }
    const char *ext = name + (len - 4U);
    return (strcasecmp(ext, ".mp3") == 0);
}

static esp_err_t make_absolute_folder(const char *folder, char *out, size_t out_len)
{
    if (folder == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *mount = storage_manager_mount_point();
    if (mount == NULL || mount[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    if (folder[0] == '/') {
        if (strncmp(folder, mount, strlen(mount)) == 0) {
            int n = snprintf(out, out_len, "%s", folder);
            return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        int n = snprintf(out, out_len, "%s%s", mount, folder);
        return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    int n = snprintf(out, out_len, "%s/%s", mount, folder);
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static bool is_regular_file(const char *path)
{
    struct stat st = { 0 };
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

esp_err_t content_index_pick_random_mp3(const char *folder, char *out_path, size_t out_path_len, size_t *out_count)
{
    if (folder == NULL || out_path == NULL || out_path_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    bool mounted = false;
    esp_err_t lock_err = storage_manager_lock_for_io(&mounted);
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    if (!mounted) {
        storage_manager_unlock_for_io();
        return ESP_ERR_INVALID_STATE;
    }

    char abs_folder[CONTENT_INDEX_PATH_MAX] = { 0 };
    esp_err_t path_err = make_absolute_folder(folder, abs_folder, sizeof(abs_folder));
    if (path_err != ESP_OK) {
        storage_manager_unlock_for_io();
        return path_err;
    }

    DIR *dir = opendir(abs_folder);
    if (dir == NULL) {
        storage_manager_unlock_for_io();
        return ESP_ERR_NOT_FOUND;
    }

    size_t count = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (!has_mp3_extension(ent->d_name)) {
            continue;
        }
        ++count;
    }
    closedir(dir);

    if (out_count != NULL) {
        *out_count = count;
    }
    if (count == 0U) {
        storage_manager_unlock_for_io();
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t pick = esp_random() % (uint32_t)count;
    size_t idx = 0;

    dir = opendir(abs_folder);
    if (dir == NULL) {
        storage_manager_unlock_for_io();
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (!has_mp3_extension(ent->d_name)) {
            continue;
        }
        if (idx++ != (size_t)pick) {
            continue;
        }

        int n = snprintf(out_path, out_path_len, "%s/%s", abs_folder, ent->d_name);
        if (n <= 0 || (size_t)n >= out_path_len) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (!is_regular_file(out_path)) {
            result = ESP_ERR_NOT_FOUND;
            break;
        }

        result = ESP_OK;
        break;
    }

    closedir(dir);
    storage_manager_unlock_for_io();
    return result;
}
