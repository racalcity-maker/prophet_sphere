#include "audio_asset_registry.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include "config_manager.h"
#include "content_index.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_tags.h"
#include "sdkconfig.h"
#include "storage_manager.h"

#ifndef CONFIG_ORB_AUDIO_ASSET1_PATH
#define CONFIG_ORB_AUDIO_ASSET1_PATH "/sdcard/1.mp3"
#endif
#ifndef CONFIG_ORB_AUDIO_ASSET2_PATH
#define CONFIG_ORB_AUDIO_ASSET2_PATH "/sdcard/2.mp3"
#endif
#ifndef CONFIG_ORB_AUDIO_ASSET3_PATH
#define CONFIG_ORB_AUDIO_ASSET3_PATH "/sdcard/3.mp3"
#endif
#ifndef CONFIG_ORB_AUDIO_GRUMBLE_DIR
#define CONFIG_ORB_AUDIO_GRUMBLE_DIR "/sdcard/audio/grumble"
#endif
#ifndef CONFIG_ORB_AUDIO_LOTTERY_SORTING_DIR
#define CONFIG_ORB_AUDIO_LOTTERY_SORTING_DIR "/sdcard/audio/lottery/sorting"
#endif
#ifndef CONFIG_ORB_AUDIO_LOTTERY_TEAM1_DIR
#define CONFIG_ORB_AUDIO_LOTTERY_TEAM1_DIR "/sdcard/audio/lottery/team1"
#endif
#ifndef CONFIG_ORB_AUDIO_LOTTERY_TEAM2_DIR
#define CONFIG_ORB_AUDIO_LOTTERY_TEAM2_DIR "/sdcard/audio/lottery/team2"
#endif
#ifndef CONFIG_ORB_AUDIO_LOTTERY_TEAM3_DIR
#define CONFIG_ORB_AUDIO_LOTTERY_TEAM3_DIR "/sdcard/audio/lottery/team3"
#endif
#ifndef CONFIG_ORB_AUDIO_LOTTERY_TEAM4_DIR
#define CONFIG_ORB_AUDIO_LOTTERY_TEAM4_DIR "/sdcard/audio/lottery/team4"
#endif
#ifndef CONFIG_ORB_AUDIO_LOTTERY_FINISHED_PATH
#define CONFIG_ORB_AUDIO_LOTTERY_FINISHED_PATH "/sdcard/audio/lottery/finished.mp3"
#endif
#ifndef CONFIG_ORB_AUDIO_PROPHECY_ROOT_DIR
#define CONFIG_ORB_AUDIO_PROPHECY_ROOT_DIR "/sdcard/audio/oracle"
#endif
#ifndef CONFIG_ORB_AUDIO_HYBRID_START_DIR
#define CONFIG_ORB_AUDIO_HYBRID_START_DIR "/sdcard/audio/hybrid/start"
#endif
#ifndef CONFIG_ORB_AUDIO_HYBRID_FAIL_DIR
#define CONFIG_ORB_AUDIO_HYBRID_FAIL_DIR "/sdcard/audio/hybrid/fail"
#endif
#ifndef CONFIG_ORB_OFFLINE_GRUMBLE_ASSET_ID
#define CONFIG_ORB_OFFLINE_GRUMBLE_ASSET_ID 3
#endif

#if defined(CONFIG_ORB_OFFLINE_GRUMBLE_ASSET_ID)
#define ORB_OFFLINE_GRUMBLE_ASSET_ID_U32 ((uint32_t)CONFIG_ORB_OFFLINE_GRUMBLE_ASSET_ID)
#else
#define ORB_OFFLINE_GRUMBLE_ASSET_ID_U32 UINT32_C(3)
#endif

static SemaphoreHandle_t s_dyn_mutex;
static const char *TAG = LOG_TAG_AUDIO;

#define AUDIO_ASSET_PATH_MAX 384U
#define FOLDER_HISTORY_MAX 128U

static char s_dynamic_slot1_path[AUDIO_ASSET_PATH_MAX];
static char s_dynamic_slot2_path[AUDIO_ASSET_PATH_MAX];

typedef struct {
    uint32_t hashes[FOLDER_HISTORY_MAX];
    size_t count;
} folder_history_t;

static folder_history_t s_hist_lottery_sorting;
static folder_history_t s_hist_lottery_team1;
static folder_history_t s_hist_lottery_team2;
static folder_history_t s_hist_lottery_team3;
static folder_history_t s_hist_lottery_team4;
static folder_history_t s_hist_hybrid_start;
static folder_history_t s_hist_hybrid_fail;

#define PROPHECY_ASSET_FIRST ((uint32_t)AUDIO_ASSET_PROPHECY_GREET_CHOICE)
#define PROPHECY_ASSET_LAST ((uint32_t)AUDIO_ASSET_PROPHECY_FAREWELL_TIMING)
#define PROPHECY_PHASE_COUNT 4U
#define PROPHECY_ARCHETYPE_COUNT 10U
#define PROPHECY_HISTORY_SLOTS (PROPHECY_ARCHETYPE_COUNT * PROPHECY_PHASE_COUNT)
static folder_history_t s_hist_prophecy[PROPHECY_HISTORY_SLOTS];

static const char *s_prophecy_phase_names[PROPHECY_PHASE_COUNT] = {
    "greeting",
    "understanding",
    "prediction",
    "farewell",
};

static const char *s_prophecy_archetype_names[PROPHECY_ARCHETYPE_COUNT] = {
    "choice",
    "danger",
    "future",
    "inner_state",
    "love",
    "path",
    "money",
    "wish",
    "yes_no",
    "timing",
};

static esp_err_t lock_dynamic(void)
{
    if (s_dyn_mutex == NULL) {
        s_dyn_mutex = xSemaphoreCreateMutex();
        if (s_dyn_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    if (xSemaphoreTake(s_dyn_mutex, ticks > 0 ? ticks : 1) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void unlock_dynamic(void)
{
    if (s_dyn_mutex != NULL) {
        xSemaphoreGive(s_dyn_mutex);
    }
}

static bool path_copy_or_build(const char *configured, uint32_t asset_id, char *out_path, size_t out_path_len)
{
    if (out_path == NULL || out_path_len == 0) {
        return false;
    }

    if (configured != NULL && configured[0] != '\0') {
        int n = snprintf(out_path, out_path_len, "%s", configured);
        return (n > 0 && (size_t)n < out_path_len);
    }

    int n = snprintf(out_path, out_path_len, "/sdcard/%lu.mp3", (unsigned long)asset_id);
    return (n > 0 && (size_t)n < out_path_len);
}

static bool path_copy_runtime(const char *stored, char *out_path, size_t out_path_len)
{
    if (stored == NULL || stored[0] == '\0') {
        return false;
    }
    int n = snprintf(out_path, out_path_len, "%s", stored);
    return (n > 0 && (size_t)n < out_path_len);
}

static bool path_exists_regular(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    struct stat st = { 0 };
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

static bool has_mp3_extension(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t len = strlen(name);
    if (len < 4U) {
        return false;
    }
    return (strcasecmp(name + len - 4U, ".mp3") == 0);
}

static uint32_t hash_fnv1a(const char *text)
{
    const uint8_t *p = (const uint8_t *)text;
    uint32_t h = 2166136261U;
    while (p != NULL && *p != 0U) {
        h ^= (uint32_t)(*p++);
        h *= 16777619U;
    }
    return h;
}

static bool history_contains(const folder_history_t *hist, uint32_t hash)
{
    if (hist == NULL) {
        return false;
    }
    for (size_t i = 0; i < hist->count; ++i) {
        if (hist->hashes[i] == hash) {
            return true;
        }
    }
    return false;
}

static void history_reset(folder_history_t *hist)
{
    if (hist != NULL) {
        hist->count = 0U;
    }
}

static void history_add(folder_history_t *hist, uint32_t hash)
{
    if (hist == NULL || hash == 0U) {
        return;
    }
    if (history_contains(hist, hash)) {
        return;
    }
    if (hist->count >= FOLDER_HISTORY_MAX) {
        for (size_t i = 1; i < hist->count; ++i) {
            hist->hashes[i - 1U] = hist->hashes[i];
        }
        hist->count = FOLDER_HISTORY_MAX - 1U;
    }
    hist->hashes[hist->count++] = hash;
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

static esp_err_t pick_random_from_folder(const char *folder,
                                         const folder_history_t *history,
                                         bool skip_played,
                                         char *out_path,
                                         size_t out_path_len,
                                         uint32_t *out_hash,
                                         size_t *out_total,
                                         size_t *out_available)
{
    bool mounted = false;
    esp_err_t lock_err = storage_manager_lock_for_io(&mounted);
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    if (!mounted) {
        storage_manager_unlock_for_io();
        ESP_LOGW(TAG, "storage not mounted while resolving folder=%s", folder == NULL ? "(null)" : folder);
        return ESP_ERR_INVALID_STATE;
    }

    char abs_folder[AUDIO_ASSET_PATH_MAX] = { 0 };
    esp_err_t abs_err = make_absolute_folder(folder, abs_folder, sizeof(abs_folder));
    if (abs_err != ESP_OK) {
        storage_manager_unlock_for_io();
        return abs_err;
    }

    DIR *dir = opendir(abs_folder);
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir failed path=%s errno=%d", abs_folder, errno);
        storage_manager_unlock_for_io();
        return ESP_ERR_NOT_FOUND;
    }

    size_t total = 0U;
    size_t available = 0U;
    uint32_t chosen_hash = 0U;
    char chosen_path[AUDIO_ASSET_PATH_MAX] = { 0 };

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (!has_mp3_extension(ent->d_name)) {
            continue;
        }

        char full_path[AUDIO_ASSET_PATH_MAX] = { 0 };
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", abs_folder, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full_path) || !path_exists_regular(full_path)) {
            continue;
        }

        total++;
        uint32_t hash = hash_fnv1a(full_path);
        if (skip_played && history_contains(history, hash)) {
            continue;
        }

        available++;
        if ((esp_random() % (uint32_t)available) == 0U) {
            chosen_hash = hash;
            (void)snprintf(chosen_path, sizeof(chosen_path), "%s", full_path);
        }
    }
    closedir(dir);
    storage_manager_unlock_for_io();

    if (out_total != NULL) {
        *out_total = total;
    }
    if (out_available != NULL) {
        *out_available = available;
    }
    if (available == 0U || chosen_path[0] == '\0') {
        return (total == 0U) ? ESP_ERR_NOT_FOUND : ESP_ERR_NOT_FOUND;
    }

    int out_n = snprintf(out_path, out_path_len, "%s", chosen_path);
    if (out_n <= 0 || (size_t)out_n >= out_path_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (out_hash != NULL) {
        *out_hash = chosen_hash;
    }
    return ESP_OK;
}

static esp_err_t resolve_random_folder_no_repeat(const char *folder,
                                                 folder_history_t *history,
                                                 const char *label,
                                                 const char *fallback_path,
                                                 uint32_t fallback_asset_id,
                                                 char *out_path,
                                                 size_t out_path_len)
{
    size_t total = 0U;
    size_t available = 0U;
    uint32_t selected_hash = 0U;

    esp_err_t pick_err =
        pick_random_from_folder(folder, history, true, out_path, out_path_len, &selected_hash, &total, &available);
    if (pick_err == ESP_OK) {
        history_add(history, selected_hash);
        ESP_LOGD(TAG, "audio pick %s: %s (%u/%u)", label, out_path, (unsigned)available, (unsigned)total);
        return ESP_OK;
    }

    if (total > 0U) {
        history_reset(history);
        pick_err =
            pick_random_from_folder(folder, history, false, out_path, out_path_len, &selected_hash, &total, &available);
        if (pick_err == ESP_OK) {
            history_add(history, selected_hash);
            ESP_LOGD(TAG, "audio pick %s: %s (history reset)", label, out_path);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG,
             "%s folder pick failed path=%s (%s), fallback path",
             label,
             folder,
             esp_err_to_name(pick_err));
    return path_copy_or_build(fallback_path, fallback_asset_id, out_path, out_path_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t resolve_runtime_aura_path(const orb_runtime_config_t *cfg, bool intro, char *out_path, size_t out_path_len)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->offline_submode != ORB_OFFLINE_SUBMODE_AURA) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!intro && cfg->aura_selected_color[0] != '\0') {
        char color_path[AUDIO_ASSET_PATH_MAX] = { 0 };
        int n = snprintf(color_path, sizeof(color_path), "%s/%s.mp3", cfg->aura_response_dir, cfg->aura_selected_color);
        if (n > 0 && n < (int)sizeof(color_path) && path_exists_regular(color_path)) {
            int out_n = snprintf(out_path, out_path_len, "%s", color_path);
            if (out_n > 0 && (size_t)out_n < out_path_len) {
                ESP_LOGD(TAG, "audio pick response-color: %s", out_path);
                return ESP_OK;
            }
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_LOGW(TAG, "aura color response file missing for '%s', fallback to random", cfg->aura_selected_color);
    }

    size_t count = 0;
    const char *folder = intro ? cfg->aura_intro_dir : cfg->aura_response_dir;
    esp_err_t pick_err = content_index_pick_random_mp3(folder, out_path, out_path_len, &count);
    if (pick_err == ESP_OK) {
        ESP_LOGD(TAG, "audio pick %s: %s (%u files)", intro ? "intro" : "response", out_path, (unsigned)count);
    }
    return pick_err;
}

static esp_err_t resolve_grumble_path(char *out_path, size_t out_path_len)
{
    size_t count = 0;
    esp_err_t pick_err = content_index_pick_random_mp3(CONFIG_ORB_AUDIO_GRUMBLE_DIR, out_path, out_path_len, &count);
    if (pick_err == ESP_OK) {
        ESP_LOGD(TAG, "audio pick grumble: %s (%u files)", out_path, (unsigned)count);
        return ESP_OK;
    }
    ESP_LOGW(TAG,
             "grumble folder pick failed (%s), fallback to asset3 path",
             esp_err_to_name(pick_err));
    return path_copy_or_build(CONFIG_ORB_AUDIO_ASSET3_PATH, (uint32_t)AUDIO_ASSET_ERROR, out_path, out_path_len)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static esp_err_t resolve_lottery_sorting_path(char *out_path, size_t out_path_len)
{
    return resolve_random_folder_no_repeat(CONFIG_ORB_AUDIO_LOTTERY_SORTING_DIR,
                                           &s_hist_lottery_sorting,
                                           "lottery sorting",
                                           CONFIG_ORB_AUDIO_ASSET1_PATH,
                                           (uint32_t)AUDIO_ASSET_STARTUP_CHIME,
                                           out_path,
                                           out_path_len);
}

static esp_err_t resolve_hybrid_start_path(char *out_path, size_t out_path_len)
{
    return resolve_random_folder_no_repeat(CONFIG_ORB_AUDIO_HYBRID_START_DIR,
                                           &s_hist_hybrid_start,
                                           "hybrid start",
                                           CONFIG_ORB_AUDIO_ASSET1_PATH,
                                           (uint32_t)AUDIO_ASSET_HYBRID_START_PHRASE,
                                           out_path,
                                           out_path_len);
}

static esp_err_t resolve_hybrid_fail_path(char *out_path, size_t out_path_len)
{
    return resolve_random_folder_no_repeat(CONFIG_ORB_AUDIO_HYBRID_FAIL_DIR,
                                           &s_hist_hybrid_fail,
                                           "hybrid fail",
                                           CONFIG_ORB_AUDIO_ASSET3_PATH,
                                           (uint32_t)AUDIO_ASSET_HYBRID_FAIL_PHRASE,
                                           out_path,
                                           out_path_len);
}

static bool prophecy_asset_to_indices(audio_asset_id_t asset_id, uint8_t *out_archetype, uint8_t *out_phase)
{
    uint32_t id = (uint32_t)asset_id;
    if (id < PROPHECY_ASSET_FIRST || id > PROPHECY_ASSET_LAST) {
        return false;
    }

    uint32_t idx = id - PROPHECY_ASSET_FIRST;
    uint32_t archetype = idx / PROPHECY_PHASE_COUNT;
    uint32_t phase = idx % PROPHECY_PHASE_COUNT;
    if (archetype >= PROPHECY_ARCHETYPE_COUNT || phase >= PROPHECY_PHASE_COUNT) {
        return false;
    }

    if (out_archetype != NULL) {
        *out_archetype = (uint8_t)archetype;
    }
    if (out_phase != NULL) {
        *out_phase = (uint8_t)phase;
    }
    return true;
}

static esp_err_t resolve_prophecy_path(audio_asset_id_t asset_id, char *out_path, size_t out_path_len)
{
    uint8_t archetype = 0U;
    uint8_t phase = 0U;
    if (!prophecy_asset_to_indices(asset_id, &archetype, &phase)) {
        return ESP_ERR_NOT_FOUND;
    }

    char folder[AUDIO_ASSET_PATH_MAX] = { 0 };
    const char *arch_name = s_prophecy_archetype_names[archetype];
    int n = snprintf(folder,
                     sizeof(folder),
                     "%s/%s/%s",
                     CONFIG_ORB_AUDIO_PROPHECY_ROOT_DIR,
                     s_prophecy_phase_names[phase],
                     arch_name);
    if (n <= 0 || (size_t)n >= sizeof(folder)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t history_idx = ((uint32_t)archetype * PROPHECY_PHASE_COUNT) + (uint32_t)phase;
    const char *fallback = CONFIG_ORB_AUDIO_ASSET1_PATH;
    switch (phase) {
    case 1:
        fallback = CONFIG_ORB_AUDIO_ASSET2_PATH;
        break;
    case 2:
    case 3:
        fallback = CONFIG_ORB_AUDIO_ASSET3_PATH;
        break;
    default:
        break;
    }

    char label[80] = { 0 };
    (void)snprintf(label,
                   sizeof(label),
                   "prophecy %s/%s",
                   s_prophecy_phase_names[phase],
                   arch_name);

    esp_err_t err = resolve_random_folder_no_repeat(folder,
                                                    &s_hist_prophecy[history_idx],
                                                    label,
                                                    fallback,
                                                    (uint32_t)asset_id,
                                                    out_path,
                                                    out_path_len);
    return err;
}

esp_err_t audio_asset_registry_resolve_path(audio_asset_id_t asset_id, char *out_path, size_t out_path_len)
{
    if (out_path == NULL || out_path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    orb_runtime_config_t cfg = { 0 };
    (void)config_manager_get_snapshot(&cfg);

    if ((uint32_t)asset_id == ORB_OFFLINE_GRUMBLE_ASSET_ID_U32) {
        return resolve_grumble_path(out_path, out_path_len);
    }
    if ((uint32_t)asset_id >= PROPHECY_ASSET_FIRST && (uint32_t)asset_id <= PROPHECY_ASSET_LAST) {
        return resolve_prophecy_path(asset_id, out_path, out_path_len);
    }

    switch (asset_id) {
    case AUDIO_ASSET_STARTUP_CHIME:
        if (resolve_runtime_aura_path(&cfg, true, out_path, out_path_len) == ESP_OK) {
            return ESP_OK;
        }
        return path_copy_or_build(CONFIG_ORB_AUDIO_ASSET1_PATH, (uint32_t)asset_id, out_path, out_path_len)
                   ? ESP_OK
                   : ESP_ERR_INVALID_SIZE;
    case AUDIO_ASSET_ACK:
        if (resolve_runtime_aura_path(&cfg, false, out_path, out_path_len) == ESP_OK) {
            return ESP_OK;
        }
        return path_copy_or_build(CONFIG_ORB_AUDIO_ASSET2_PATH, (uint32_t)asset_id, out_path, out_path_len)
                   ? ESP_OK
                   : ESP_ERR_INVALID_SIZE;
    case AUDIO_ASSET_ERROR:
        return path_copy_or_build(CONFIG_ORB_AUDIO_ASSET3_PATH, (uint32_t)asset_id, out_path, out_path_len)
                   ? ESP_OK
                   : ESP_ERR_INVALID_SIZE;
    case AUDIO_ASSET_LOTTERY_SORTING:
        return resolve_lottery_sorting_path(out_path, out_path_len);
    case AUDIO_ASSET_HYBRID_START_PHRASE:
        return resolve_hybrid_start_path(out_path, out_path_len);
    case AUDIO_ASSET_HYBRID_RETRY_PHRASE:
    case AUDIO_ASSET_HYBRID_JOKE_PHRASE:
    case AUDIO_ASSET_HYBRID_FORBIDDEN_PHRASE:
        ESP_LOGW(TAG, "hybrid service asset id=%" PRIu32 " is disabled in remote-only mode", (uint32_t)asset_id);
        return ESP_ERR_NOT_FOUND;
    case AUDIO_ASSET_HYBRID_FAIL_PHRASE:
        return resolve_hybrid_fail_path(out_path, out_path_len);
    case AUDIO_ASSET_LOTTERY_TEAM1:
        return resolve_random_folder_no_repeat(CONFIG_ORB_AUDIO_LOTTERY_TEAM1_DIR,
                                               &s_hist_lottery_team1,
                                               "lottery team1",
                                               CONFIG_ORB_AUDIO_ASSET1_PATH,
                                               (uint32_t)asset_id,
                                               out_path,
                                               out_path_len);
    case AUDIO_ASSET_LOTTERY_TEAM2:
        return resolve_random_folder_no_repeat(CONFIG_ORB_AUDIO_LOTTERY_TEAM2_DIR,
                                               &s_hist_lottery_team2,
                                               "lottery team2",
                                               CONFIG_ORB_AUDIO_ASSET2_PATH,
                                               (uint32_t)asset_id,
                                               out_path,
                                               out_path_len);
    case AUDIO_ASSET_LOTTERY_TEAM3:
        return resolve_random_folder_no_repeat(CONFIG_ORB_AUDIO_LOTTERY_TEAM3_DIR,
                                               &s_hist_lottery_team3,
                                               "lottery team3",
                                               CONFIG_ORB_AUDIO_ASSET2_PATH,
                                               (uint32_t)asset_id,
                                               out_path,
                                               out_path_len);
    case AUDIO_ASSET_LOTTERY_TEAM4:
        return resolve_random_folder_no_repeat(CONFIG_ORB_AUDIO_LOTTERY_TEAM4_DIR,
                                               &s_hist_lottery_team4,
                                               "lottery team4",
                                               CONFIG_ORB_AUDIO_ASSET3_PATH,
                                               (uint32_t)asset_id,
                                               out_path,
                                               out_path_len);
    case AUDIO_ASSET_LOTTERY_FINISHED:
        return path_copy_or_build(CONFIG_ORB_AUDIO_LOTTERY_FINISHED_PATH, (uint32_t)asset_id, out_path, out_path_len)
                   ? ESP_OK
                   : ESP_ERR_INVALID_SIZE;
    case AUDIO_ASSET_DYNAMIC_SLOT1:
    case AUDIO_ASSET_DYNAMIC_SLOT2: {
        esp_err_t lock_err = lock_dynamic();
        if (lock_err != ESP_OK) {
            return lock_err;
        }
        const char *stored = (asset_id == AUDIO_ASSET_DYNAMIC_SLOT1) ? s_dynamic_slot1_path : s_dynamic_slot2_path;
        bool ok = path_copy_runtime(stored, out_path, out_path_len);
        unlock_dynamic();
        return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    case AUDIO_ASSET_NONE:
    default:
        return ESP_ERR_NOT_FOUND;
    }
}

esp_err_t audio_asset_registry_set_dynamic_path(audio_asset_id_t slot_id, const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot_id != AUDIO_ASSET_DYNAMIC_SLOT1 && slot_id != AUDIO_ASSET_DYNAMIC_SLOT2) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t lock_err = lock_dynamic();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    char *dst = (slot_id == AUDIO_ASSET_DYNAMIC_SLOT1) ? s_dynamic_slot1_path : s_dynamic_slot2_path;
    size_t dst_len = (slot_id == AUDIO_ASSET_DYNAMIC_SLOT1) ? sizeof(s_dynamic_slot1_path) : sizeof(s_dynamic_slot2_path);
    int n = snprintf(dst,
                     dst_len,
                     "%s",
                     path);
    unlock_dynamic();
    return (n > 0 && (size_t)n < dst_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t audio_asset_registry_clear_dynamic_paths(void)
{
    esp_err_t lock_err = lock_dynamic();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    s_dynamic_slot1_path[0] = '\0';
    s_dynamic_slot2_path[0] = '\0';
    unlock_dynamic();
    return ESP_OK;
}
