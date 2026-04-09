# Oracle: Архитектура и зависимости

Актуально для текущего состояния репозитория (ветка `main`).

## 1) Слои архитектуры

- `main/app_main.c`: bootstrap, инициализация рантайма, запуск control-task.
- `app_core`: единая orchestration-точка (events, mode switch, dispatch, session/sequence).
- `service_runtime`: централизованный lifecycle shared-сервисов по требованиям режима.
- `services_*`: изолированные владельцы железа/сети/интеграций.
- `modes`: бизнес-сценарии (`offline_scripted`, `hybrid_ai`, `installation_slave`).
- `services_web` + portal JS: операторский UI и REST/WS API.
- `kws/pi_ws`: сервер распознавания/озвучки на Raspberry Pi.

## 2) Модель владения (ownership)

- Только `app_control_task` принимает системные решения (mode, session, action routing).
- Сервисы владеют своим runtime-состоянием через свои task/worker:
  - touch -> `touch_task`
  - led -> `led_task`
  - audio -> `audio_task` + `audio_worker*`
  - mic -> `mic_task` + `mic_ws_client*`/`mic_task_*`
- Web/network callbacks не должны напрямую управлять hardware, только через service/app API.

## 3) Карта зависимостей ESP-IDF компонентов

Источник истины: `components/*/CMakeLists.txt`.

| Компонент | Назначение | REQUIRES | PRIV_REQUIRES |
|---|---|---|---|
| `common` | Общие типы, intent/mem monitor | `esp_timer` | - |
| `app_core` | FSM, mode manager, dispatch, runtime guard | `freertos`, `common`, `modes`, `services_config` | `services_audio` |
| `bsp` | Пины/кнопки платы | `common`, `app_core`, `freertos`, `esp_driver_gpio` | - |
| `modes` | Логика режимов и submode | `common`, `services_config` | - |
| `service_runtime` | Lifecycle orchestration сервисов | `freertos`, `common`, `app_core`, `services_touch`, `services_led`, `services_audio`, `services_mic`, `services_ai`, `services_storage`, `services_network`, `services_mqtt`, `services_web`, `services_ota` | - |
| `services_config` | Runtime config + NVS store | `freertos`, `common`, `nvs_flash` | - |
| `services_touch` | Touch service/task | `app_core`, `freertos`, `common`, `driver` | - |
| `services_led` | LED service/task/effects/output | `app_core`, `freertos`, `common`, `services_config`, `bsp`, `esp_driver_rmt` | - |
| `services_audio` | Audio service/task/worker/mixer/bg/codec | `app_core`, `freertos`, `common`, `bsp`, `services_config`, `services_storage`, `esp_driver_i2s`, `esp_driver_gpio`, `heap` | - |
| `services_mic` | Mic service/task/ws/i2s | `app_core`, `freertos`, `common`, `heap`, `esp_driver_i2s`, `esp_event`, `esp_websocket_client` | - |
| `services_storage` | SD mount/content index | `common`, `freertos`, `fatfs`, `sdmmc`, `esp_driver_spi`, `esp_system` | - |
| `services_network` | Wi-Fi profiles/events/policy | `app_core`, `common`, `freertos`, `services_config`, `esp_event`, `esp_netif`, `esp_wifi`, `nvs_flash` | - |
| `services_mqtt` | MQTT lifecycle | `app_core`, `common`, `freertos` | - |
| `services_web` | HTTP/HTTPS server, REST, talk WS, portal | `app_core`, `common`, `freertos`, `services_config`, `services_audio`, `services_led`, `services_mic`, `services_network`, `esp_http_server`, `esp_https_server`, `esp_http_client`, `esp_timer` | - |
| `services_ai` | AI client/task/prompt | `app_core`, `freertos`, `common` | - |
| `services_ota` | OTA service | `freertos`, `common`, `app_update` | - |

## 4) Runtime зависимости по режимам

`service_runtime` применяет требования режима и сериализует transition.

- Always-on baseline: `touch`, `led`, `audio`.
- Optional set по режимам:
  - `offline_scripted`: `mic`, `storage` + network/web (если профиль сети включен политикой).
  - `hybrid_ai`: `mic`, `storage` + network/web (если профиль сети включен политикой).
  - `installation_slave`: `mic`, `storage` + network/web (если профиль сети включен политикой).

Сетевой профиль вычисляется политикой (`services_network`), а не веб-слоем.

## 5) Web/Portal зависимости

- C API слой:
  - `web/web_server.c`, `web/web_portal.c`
  - `api/core/*`
  - `api/endpoints/*`
  - `api/talk/*`
- Embedded assets в `services_web/CMakeLists.txt`:
  - `portal/pages/*.html`
  - `portal/assets/css/app.css`
  - `portal/assets/js/core/dom_http.js`
  - `portal/assets/js/features/*.js`
  - `portal/assets/js/pages/*.js`
  - `portal/assets/js/app.js`
- HTTPS сертификаты:
  - `components/services_web/certs/servercert.pem`
  - `components/services_web/certs/prvtkey.pem`

## 6) Внешние зависимости (Raspberry Pi WS сервер)

Базовые Python зависимости (`kws/pi_ws/requirements.txt`):

- `numpy>=1.26`
- `websockets>=14.0`
- `vosk>=0.3.45`
- `omegaconf>=2.3`

Опциональные backend-зависимости:

- `silero`: `torch` (+ модель через `torch.hub`).
- `piper`: внешний бинарник `piper` + `.onnx` voice model.
- `yandex`: доступ к облачному API, ключ/токен и folder id.
- `local llm` (опционально): `llama.cpp` сервер + GGUF модель.

## 7) Конфиги и источники правды

- Сборочные дефолты:
  - `sdkconfig.defaults` (production baseline)
  - `sdkconfig.defaults.dev` (dev overlay)
- Runtime конфиг устройства: NVS (`services_config`).
- Runtime TTS конфиг сервера: `kws/pi_ws/tts_runtime_config.json`.
- Текстовый банк oracle v2:
  - `docs/oracle_texts_jsons/manifest.json`
  - `docs/oracle_texts_jsons/answers/*`
  - `docs/oracle_texts_jsons/service/*`
  - `docs/oracle_texts_jsons/dictionaries/*`

## 8) Быстрая проверка целостности зависимостей

- При добавлении нового cross-component include:
  1. Обновить `REQUIRES`/`PRIV_REQUIRES` в нужном `CMakeLists.txt`.
  2. Проверить ownership: новый вызов не должен обходить `app_core`/service API.
  3. Для web endpoints: убедиться, что не добавляется прямая hardware-логика в HTTP handler.
  4. Для mode/runtime: изменения должны проходить через `service_runtime` и mode actions.

