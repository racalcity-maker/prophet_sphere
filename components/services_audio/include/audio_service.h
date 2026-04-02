#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <stdint.h>
#include "esp_err.h"
#include "audio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Queue-based and thread-safe frontend.
 * audio_task is the only owner of playback/hardware state.
 * Multi-step interaction sequencing (track chains, gaps, mode timing)
 * belongs to app_core orchestration, not to this service.
 */
esp_err_t audio_service_init(void);
esp_err_t audio_service_start_task(void);
esp_err_t audio_service_stop_task(void);
esp_err_t audio_service_play_asset(audio_asset_id_t asset_id, uint32_t timeout_ms);
esp_err_t audio_service_stop(uint32_t timeout_ms);
esp_err_t audio_service_set_volume(uint8_t volume, uint32_t timeout_ms);
esp_err_t audio_service_set_dynamic_asset_path(audio_asset_id_t slot_id, const char *path);
esp_err_t audio_service_clear_dynamic_asset_paths(void);

#ifdef __cplusplus
}
#endif

#endif
