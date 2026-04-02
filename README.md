# Interactive Talking Orb (ESP-IDF 5.3.x)

## Overview
This project is a modular ESP32-S3 baseline for an interactive talking orb device.
It provides a safe FreeRTOS ownership and queue architecture first, with hardware/network logic intentionally stubbed for incremental implementation.

## Concurrency And Ownership Model
- `app_control_task`: only owner of global orchestration, FSM, session lifecycle, mode transitions.
- `touch_task`: only producer of normalized touch events.
- `led_task`: only owner of LED render state/hardware updates.
- `audio_task`: only owner of audio playback/output state.
- `mic_task`: only owner of microphone capture/I2S RX state.
- `ai_task`: only owner of AI request execution state.
- Web/MQTT/network callbacks are queue producers only; they do not directly control hardware-facing services.

## Queue Model
- `app_event_queue`: control/event backbone (`app_control_task` consumer).
- `led_cmd_queue`: `led_task` command inbox.
- `audio_cmd_queue`: `audio_task` command inbox.
- `ai_cmd_queue`: `ai_task` command inbox.
- `mic_cmd_queue` (service-local): `mic_task` command inbox.
- Queue send/receive operations use bounded timeouts from menuconfig (`Orb Device Configuration`).

## Service Runtime Layer
- `service_runtime` is the centralized lifecycle orchestrator for shared services.
- `mode_manager` applies mode requirements through a control-context runtime hook during mode switch.
- Shared-service lifecycle states:
  `UNINITIALIZED -> STOPPED -> STARTING -> RUNNING -> STOPPING -> ERROR`.
- `touch/led/audio` are policy-level always-on shared services and stay running across all modes.
- Optional services are managed by mode requirements (`mic/ai/network/mqtt/web/ota/storage`).

## Component Responsibilities
- `common`: shared enums, error codes, log tag constants.
- `bsp`: board name/pin abstraction from `CONFIG_*`.
- `app_core`: events, tasking, FSM, mode manager, session controller, interaction sequence orchestrator.
- `service_runtime`: mode-driven shared-service start/stop policy and sequencing.
- `services_config`: runtime config snapshot with NVS persistence (`orb_cfg` namespace).
- `services_touch`: real ESP32-S3 capacitive touch producer task (4 zones) with calibration/filter/debounce.
- `services_led`: queue-based LED command frontend + `led_task` renderer + WS2812 RMT backend.
  Includes a software current limiter (default 2A) and touch-zone brightness overlay (effect colors preserved).
  Current matrix scenes: `idle_breathe`, `touch_awake`, `error_flash`, `fire2012`, `plasma`, `sparkle`, `color_wave`.
- `services_audio`: queue-based audio frontend + `audio_task` loop + `audio_worker` playback state machine + selectable I2S DAC backend (`PCM5102A`/`MAX98357A`).
- `services_mic`: queue-based microphone frontend + `mic_task` owner + I2S RX (`INMP441`) capture backend with timed capture and level metrics events.
  KWS inference is executed by a dedicated `mic_kws_worker` task (optional core affinity split from `mic_task`).
  Recommended dual-core split: `mic_task` on core 0, `mic_kws_worker` on core 1.
  KWS backend is now selectable: current custom float DS-CNN or TFLM int8 scaffold.
- `services_storage`: SD card storage service (SPI) with mount/unmount and file-read helpers used by audio MP3 playback.
- `services_ota`: OTA lifecycle service (partition awareness + safe startup hooks).
- `services_network`: real Wi-Fi lifecycle manager (`STA` / `SoftAP`) with mode-aware profile policy.
  It posts only `APP_EVENT_NETWORK_UP/DOWN` into `app_event_queue` and does not bypass control ownership.
- `services_mqtt`: MQTT lifecycle service, posts MQTT command events.
- `services_ai`: queue-based AI frontend/task + prompt engine.
- `services_web`: local HTTP server (`esp_http_server`) with queue-safe REST control endpoints.
- `modes`: offline/hybrid/installation mode implementations behind `app_mode_t`.
  Offline submodes are centralized under `modes/mode_offline_scripted/`:
  `offline_submode_router.c` + one file per submode (`aura`, `lottery`, `prophecy`).

## File-Level Decomposition
- Service public APIs stay in `*_service.c` / client-manager files.
- Worker loops that own task state are separated into `*_task.c` (audio, led, touch, mic, ai).
- Audio worker internals are now split by responsibility:
  `audio_worker.c` (orchestration/dispatch), `audio_worker_mixer.c` (FG+BG mix/write),
  `audio_worker_bg.c` (background WAV lifecycle/fade), `audio_worker_reactive.c` (audio-level event bridge).
- LED scene/effect rendering logic is separated into `services_led/led_effects.c`.
- Backend adapters stay in dedicated files (for example `audio_output_i2s.c`).
- Config defaults are separated from runtime config API (`config_defaults.c` vs `config_manager.c`).
- `app_core` keeps queue/event/FSM/mode/session separation, with `app_control_task` bootstrap split from queue setup.
- Offline submode logic is no longer mixed in one file:
  `mode_offline_scripted.c` is now a thin entry point, while submode behavior lives in dedicated strategy files.

## Build (ESP-IDF 5.3.x)
1. Install and export ESP-IDF 5.3.x environment.
2. Configure:
   - `idf.py set-target esp32s3`
   - `idf.py menuconfig`
3. Build:
   - `idf.py build`
4. Flash/monitor:
   - `idf.py -p <PORT> flash monitor`

## Menuconfig
Root menu: `Orb Device Configuration`
- Board Configuration
- Touch Configuration
- LED Configuration
- Audio Configuration
- Storage Configuration
- Network Configuration
- MQTT Configuration
- AI Configuration
- Web Configuration
- OTA Configuration
- Mode Configuration
- Tasking and Queue Configuration

Board mode button (active-low to GND):
- `Board Configuration -> Mode Button Configuration -> Enable hardware mode button`
- `Board Configuration -> Mode Button Configuration -> Mode button GPIO`
- `Board Configuration -> Mode Button Configuration -> Button poll period / debounce`
- `Board Configuration -> Mode Button Configuration -> Task stack / priority`
- Press action cycles modes:
  `offline_scripted -> hybrid_ai -> installation_slave -> offline_scripted`

Board submode button (active-low to GND):
- `Board Configuration -> Submode Button Configuration -> Enable hardware submode button`
- `Board Configuration -> Submode Button Configuration -> Submode button GPIO`
- `Board Configuration -> Submode Button Configuration -> Button poll period / debounce`
- `Board Configuration -> Submode Button Configuration -> Task stack / priority`
- Press action is mode-specific:
  - `offline_scripted`: cycles offline submodes `aura -> lottery -> prophecy -> aura`
  - `hybrid_ai`: cycles LED scene `plasma <-> sparkle`
  - `installation_slave`: cycles LED scene `color_wave <-> idle_breathe`

Offline submode at startup:
- Loaded from NVS runtime config if saved previously.
- If NVS has no value, default comes from `Config Configuration -> Offline Runtime Defaults`.

Network profile policy by mode is configurable in:
- `Network Configuration -> Offline Mode Network Policy`
- `Network Configuration -> Hybrid AI Mode Network Policy`
- `Network Configuration -> Installation Mode Network Policy`

For local offline web access (no external internet):
- set offline mode policy to `Local SoftAP only`
- configure `SoftAP SSID/password/channel` in `Network Configuration`

Current defaults:
- `offline_scripted` -> `SoftAP`
- `hybrid_ai` -> `STA`
- `installation_slave` -> `STA` (with MQTT/web/OTA enabled by runtime requirements, AI disabled)

For offline aura 2-track flow:
- hold event still comes from `Touch Configuration -> ORB_TOUCH_HOLD_EVENT_ENABLE` and `ORB_TOUCH_HOLD_TIME_MS`
- track folders/submode/gap are runtime-configurable via `/api/config` (and persisted to NVS)
- current aura trigger default is 5s hold (`ORB_TOUCH_HOLD_TIME_MS=5000`)
- menuconfig values remain only as fallback defaults

Audio DAC profile selection:
- `Audio Configuration -> I2S DAC profile`
  - `PCM5102A`
  - `MAX98357A / MAX9xxxx-compatible`
- `Audio Configuration -> Enable real MP3 playback from SD (Helix)`
- `Audio Configuration -> Asset #1/#2/#3 path`
- `Audio Configuration -> MP3 input buffer size / read chunk size`
- Bring-up tone options:
  - `ORB_AUDIO_TEST_TONE_ENABLE`
  - `ORB_AUDIO_TEST_TONE_FREQ_HZ`
  - `ORB_AUDIO_TEST_TONE_LEVEL_PERCENT`
  - `ORB_AUDIO_TEST_TONE_CHUNK_MS`

SD storage bring-up options:
- `Storage Configuration -> Enable SD card storage service`
- `Storage Configuration -> Auto-mount SD on boot`
- `Storage Configuration -> SD SPI host/pins/clock`
- `Storage Configuration -> Mount point`
- `Microphone Configuration -> KWS inference backend`
  - `Custom float DS-CNN (current)` (default)
  - `TensorFlow Lite Micro int8 (scaffold)`

OTA options:
- `OTA Configuration -> Enable OTA service`
- `OTA Configuration -> Maximum OTA URL length`
- `OTA Configuration -> Allow HTTP OTA URL` (testing only, disabled by default)

Config persistence options:
- `Config Configuration -> Persist runtime config to NVS`
- `Config Configuration -> NVS namespace for runtime config`

## Flash Partition Layout
Project now uses custom `partitions.csv` with:
- `nvs` (runtime storage, including config persistence)
- `otadata` (OTA metadata)
- `ota_0`, `ota_1` (dual OTA app slots)
- `webfs` (SPIFFS partition for web assets)
- `sndfs` (SPIFFS partition for system sounds/assets)

## Current Scope
Not in active firmware flow:
- live stream decode pipeline (phone mic/websocket) and advanced audio controls (pause/seek)
- AI backend networking
- full speech-to-text dialogue pipeline (cloud/local ASR + NLP routing)
- OTA download/apply workflow

Current hybrid microphone path includes on-device word-KWS inference:
- trained DS-CNN checkpoint is exported into C arrays and linked into firmware
- `mic_task` captures PCM, runs KWS in `mic_intent_model`, and posts `intent_id/confidence`
- `hybrid_ai` mode maps detected intent to prophecy archetype, with fallback when confidence is low

KWS model export command (run after retraining):
`python kws/scripts/export_kws_word_to_mic_c.py --checkpoint kws/runs/kws_word_ru_v3/kws_best.pt --word-intent-map kws/manifests_word_v2/word_to_intent.json --out-header components/services_mic/include/mic_kws_word_model.h --min-conf-permille 540`

Raspberry Pi WS mic bridge (optional):
- Run server:
  `python kws/scripts/pi_mic_ws_server.py --host 0.0.0.0 --port 8765 --vosk-model ~/orb_ws/models/vosk-model-small-ru-0.22 --tts-backend piper`
- ESP WS URL should point to Pi:
  `ws://<pi_ip>:8765/mic`
- Server handles Vosk intent inference and streams synthesized TTS back to ESP in real time.

Current web layer is mode-first and functional:
- `GET /` -> redirect to current mode home (`/offline|/hybrid|/installation`)
- `GET /mode` -> redirect to current mode home
- `GET /offline`, `GET /hybrid`, `GET /installation`
- Each mode page has tabs: `Home`, `Talk`, `Settings`
- `GET /health`
- `GET /api/status`
- `POST /api/mode/switch?mode=offline_scripted|hybrid_ai|installation_slave` (service API; not exposed in UI)
- `POST /api/audio/play?asset=<id>`
- `POST /api/audio/stop`
- `POST /api/led/scene?scene=<id>&duration_ms=<ms>`
- `POST /api/led/brightness?value=0..255`
- `GET /api/offline/state`
- `GET /api/offline/config`
- `POST /api/offline/config?...` for runtime offline settings:
  `submode`, `fg_volume`, `aura_gap_ms`, `aura_intro_dir`, `aura_response_dir`,
  `prophecy_gap12_ms`, `prophecy_gap23_ms`, `prophecy_gap34_ms`, `prophecy_leadin_wait_ms`,
  `prophecy_bg_gain_permille`, `prophecy_bg_fade_in_ms`, `prophecy_bg_fade_out_ms`,
  `save`
- `POST /api/offline/submode?name=<aura|lottery|prophecy>`
- `POST /api/offline/action?name=lottery_start|audio_stop`
- `GET /api/config` and `POST /api/config?...` remain for broader system config.

Web decomposition (current):
- `web_server.c`: HTTP lifecycle only.
- `web_portal.c`: page/static route registration only.
- `rest_api.c`: orchestrator and module registration only.
- `rest_api_core.c|mode.c|led.c|audio.c|config.c|network.c|offline.c`: API handlers by domain.
- `rest_api_common.c`: shared JSON/query helpers.
- Web API response buffers are shared and allocated once at startup, with PSRAM-first policy (`ORB_WEB_USE_PSRAM_BUFFERS`) and internal-RAM fallback.
- `portal/pages/*.html`, `portal/assets/css/app.css`, `portal/assets/js/app.js`: UI files separated from C code.

## Vertical MVP Flow (Offline Scripted)
The first end-to-end control-flow slice is now wired for validation:
1. Boot initializes all components and activates default mode `offline_scripted`.
2. `touch_task` calibrates 4 real capacitive zones, then monitors the ESP32-S3 touch peripheral.
3. `app_control_task` routes event through FSM and mode manager.
4. `offline_scripted` mode waits for `TOUCH_HOLD`, then emits start action for a 2-track aura sequence.
5. First track is picked from `aura_intro_dir` (random), then before second track a random color is selected from palette and written to runtime config (`aura_selected_color`).
6. `interaction_sequence` applies color stage before track #2: LED switches to `aura_color_breathe` with ramp (gap duration), audio track #2 resolves to `<aura_response_dir>/<color>.mp3` (fallback to random file if missing).
7. `audio_task` only executes `PLAY/STOP` and posts `APP_EVENT_AUDIO_DONE` per finished track.
8. `interaction_sequence` consumes first `AUDIO_DONE`, waits runtime `aura_gap_ms`, starts second track, and only then allows completion to reach mode logic.
9. After second track: mode waits 5s, performs 1s LED aura fade-out, then returns to idle.

Expected serial log markers:
- `mode changed to offline_scripted`
- `touch calibration complete`
- `TOUCH_DOWN zone=...`
- `aura hold -> start sequence`
- `audio sequence started first=... second=... gap=...`
- `aura color selected: ...`
- `PLAY_ASSET id=...`
- `mp3 playback complete id=...` (twice) or simulated fallback markers
- `aura track2 done -> post delay`
- `aura post delay complete -> fade out`
- `aura fade complete -> idle`
- `session end ... state=IDLE`

Touch tuning is available in `Orb Device Configuration -> Touch Configuration`:
- zone channel selection (`TOUCH_ZONE0..3_CHANNEL`)
- threshold percent/min delta
- release threshold percent (relative to touch threshold)
- debounce/release debounce counts
- hold timing
- calibration sample count
- filtering and baseline smoothing
- optional low-rate diagnostics logging

Touch normalization model (current):
- Each zone is processed independently (multi-touch supported).
- `TOUCH_DOWN` is emitted per zone after debounce.
- `TOUCH_UP` is emitted per zone when that same zone drops below release threshold.
- Touch detection uses absolute deviation from baseline (`|filtered-baseline|`) to support both signal polarities.

## Recommended Next Implementation Order
1. Add touch-zone overlay and 5 matrix effects (`fire2012` included) inside `led_task`.
2. Implement audio output path and completion/error reporting in `audio_task`.
3. Implement MQTT transport client and topic routing on top of active network profile.
4. Implement AI transport request lifecycle and cancellation semantics.
5. Add config schema migration/version handling and robust integration tests for queue pressure and mode switch safety.
