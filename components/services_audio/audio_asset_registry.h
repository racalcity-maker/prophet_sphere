#ifndef AUDIO_ASSET_REGISTRY_H
#define AUDIO_ASSET_REGISTRY_H

#include <stddef.h>
#include "audio_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maps logical audio asset IDs to concrete MP3 file paths.
 * Dynamic slots are runtime-configurable and used by mode orchestration.
 */
esp_err_t audio_asset_registry_resolve_path(audio_asset_id_t asset_id, char *out_path, size_t out_path_len);
esp_err_t audio_asset_registry_set_dynamic_path(audio_asset_id_t slot_id, const char *path);
esp_err_t audio_asset_registry_clear_dynamic_paths(void);

#ifdef __cplusplus
}
#endif

#endif
