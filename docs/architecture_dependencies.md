# Oracle Architecture And Dependencies

Current branch source of truth:
- Component dependencies: `components/*/CMakeLists.txt`
- Main app wiring: `main/CMakeLists.txt`

## 1) Layering

- `main/app_main.c`: thin entrypoint (`orb_bootstrap_start()`).
- `main/bootstrap/*`: startup decomposition and mode->runtime policy resolver.
- `control_bus`: shared command/event transport (`app_tasking`, `app_events`, queue ownership) plus `service_lifecycle_guard`.
- `app_core`: orchestration only (FSM, mode manager, session/interaction logic, `app_control_task` startup), plus external facades (`app_api`, `app_media_gateway`).
- `service_runtime`: lifecycle executor for already-resolved runtime plans.
- `services_*`: hardware/network/integration service owners.
- `modes`: business scenario logic (`offline_scripted`, `hybrid_ai`, `installation_slave`).
- `services_web`: local portal + REST/WS surface.
- `kws/pi_ws`: Raspberry Pi speech server.

## 2) Ownership Model

- Only `app_control_task` drives global orchestration/FSM decisions.
- Services own their runtime state in their own task/worker loops:
  - touch -> `touch_task`
  - led -> `led_task`
  - audio -> `audio_task` + `audio_worker*`
  - mic -> `mic_task` + `mic_ws_client*`
- Web/network callbacks are producers only; they do not directly own hardware state.

## 3) Component Dependency Map

| Component | Responsibility | REQUIRES |
|---|---|---|
| `common` | Shared types/enums/log tags | `esp_timer` |
| `control_bus` | Cross-component event/command bus + lifecycle guard primitive | `freertos`, `common`, `heap` |
| `app_core` | FSM + mode orchestration + control task bootstrap + external facades (`app_api`, `app_media_gateway`) | `freertos`, `common`, `control_bus`, `modes`, `services_config`, `services_network` |
| `bsp` | Board pins/buttons abstraction | `common`, `freertos`, `esp_driver_gpio` |
| `modes` | Mode/submode behavior | `common`, `services_config` |
| `service_runtime` | Shared-service lifecycle executor (start/stop/rollback for runtime plans) | `freertos`, `common`, `control_bus`, `services_touch`, `services_led`, `services_audio`, `services_mic`, `services_ai`, `services_storage`, `services_network`, `services_mqtt`, `services_web`, `services_ota` |
| `services_config` | Runtime config + NVS | `freertos`, `common`, `nvs_flash` |
| `services_touch` | Touch input task/service | `control_bus`, `freertos`, `common`, `driver` |
| `services_led` | LED task/effects/output | `control_bus`, `freertos`, `common`, `services_config`, `bsp`, `esp_driver_rmt` |
| `services_audio` | Audio task/worker/mixer/output | `control_bus`, `freertos`, `common`, `bsp`, `services_config`, `services_storage`, `esp_driver_i2s`, `esp_driver_gpio`, `heap` |
| `services_mic` | Mic capture/ws/tts pipeline | `control_bus`, `freertos`, `common`, `heap`, `esp_driver_i2s`, `esp_event`, `esp_websocket_client` |
| `services_storage` | SD storage | `common`, `freertos`, `fatfs`, `sdmmc`, `esp_driver_spi`, `esp_system` |
| `services_network` | Wi-Fi manager/profile/policy | `control_bus`, `common`, `freertos`, `services_config`, `esp_event`, `esp_netif`, `esp_wifi`, `nvs_flash` |
| `services_mqtt` | MQTT service | `control_bus`, `common`, `freertos` |
| `services_web` | HTTPS server + REST/portal/talk | `app_core`, `control_bus`, `common`, `freertos`, `services_config`, `services_network`, `esp_http_server`, `esp_https_server`, `esp_http_client`, `esp_timer` |
| `services_ai` | AI task/client | `control_bus`, `freertos`, `common` |
| `services_ota` | OTA lifecycle | `freertos`, `common`, `app_update` |

`main` currently requires:
- `nvs_flash`, `bsp`, `common`, `control_bus`, `app_core`, `service_runtime`, `services_config`, `services_web`, `services_ota`.

## 4) Runtime Contract Notes

- `control_bus` owns queue/event transport only.
- `app_core` owns orchestration bootstrap and runs `app_control_task`.
- Runtime policy is resolved above `service_runtime` (`main/bootstrap/orb_mode_runtime_policy.c`), then passed down as plan into lifecycle executor.
- `service_runtime` executes the plan only (set profile + apply requirements + rollback on failure).
- Web/transport adapters split by intent:
  - application/use-case path -> `app_api`
  - low-level realtime/media path -> `app_media_gateway`

## 5) Quick Change Checklist

1. For new cross-component includes, update `REQUIRES`/`PRIV_REQUIRES` in the component `CMakeLists.txt`.
2. Keep orchestration decisions in `app_core`, not in service callbacks.
3. Keep HTTP handlers queue-safe; avoid direct hardware ownership in `services_web`.
4. For mode/runtime behavior changes, route through `mode_manager` + `service_runtime`.
