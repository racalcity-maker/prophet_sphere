#ifndef CONTENT_INDEX_H
#define CONTENT_INDEX_H

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Storage content helper.
 * Picks a random MP3 file path from a folder on mounted storage.
 * Input folder may be absolute ("/sdcard/...") or relative ("audio/...").
 */
esp_err_t content_index_pick_random_mp3(const char *folder, char *out_path, size_t out_path_len, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif
