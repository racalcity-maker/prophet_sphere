# Запуск Voice WS сервера (Raspberry Pi)

Этот файл фиксирует рабочие команды запуска `kws/scripts/pi_mic_ws_server.py`.

## 1) Базовый запуск (без LLM, Silero TTS)

```bash
cd ~/orb_ws
. .venv/bin/activate

LD_LIBRARY_PATH=~/orb_ws/piper:~/orb_ws/piper/lib:$LD_LIBRARY_PATH \
python kws/scripts/pi_mic_ws_server.py \
  --host 0.0.0.0 --port 8765 \
  --vosk-model ~/orb_ws/models/vosk-model-small-ru-0.22 \
  --tts-backend silero \
  --silero-language ru \
  --silero-speaker-model v4_ru \
  --silero-speaker xenia \
  --silero-sample-rate 24000 \
  --silero-device cpu \
  --silero-num-threads 1 \
  --oracle-texts-dir ~/orb_ws/docs/texts \
  --oracle-texts-format auto \
  --oracle-texts-legacy-compat \
  --tts-pacing realtime \
  --tts-tempo-scale 0.85 \
  --tts-phrase-pause-ms 1500 \
  --piper-pitch-scale 1.30 \
  --tts-fx-preset mystic \
  --tts-echo-mix 0.24 \
  --tts-echo-delay-ms 220 \
  --tts-echo-feedback 0.46 \
  --tts-reverb-mix 0.36 \
  --tts-reverb-room-scale 1.35 \
  --tts-reverb-damp 0.26
```

## 2) Запуск с локальной Oracle LLM (опционально)

Если поднят `llama.cpp` OpenAI-compatible endpoint:

```bash
cd ~/orb_ws
. .venv/bin/activate

LD_LIBRARY_PATH=~/orb_ws/piper:~/orb_ws/piper/lib:$LD_LIBRARY_PATH \
python kws/scripts/pi_mic_ws_server.py \
  --host 0.0.0.0 --port 8765 \
  --vosk-model ~/orb_ws/models/vosk-model-small-ru-0.22 \
  --tts-backend silero \
  --silero-language ru \
  --silero-speaker-model v4_ru \
  --silero-speaker xenia \
  --silero-sample-rate 24000 \
  --oracle-texts-dir ~/orb_ws/docs/texts \
  --oracle-texts-format auto \
  --oracle-texts-legacy-compat \
  --tts-pacing realtime \
  --tts-tempo-scale 0.85 \
  --oracle-llm-backend local \
  --oracle-llm-local-endpoint http://127.0.0.1:8080/v1/chat/completions \
  --oracle-llm-local-model qwen2.5-1.5b-instruct-q4_k_m
```

## 3) Быстрая остановка

- В окне с сервером: `Ctrl+C`
- Проверка, что порт свободен:

```bash
ss -ltnp | grep 8765 || true
```

## 4) Формат текстов (новый + совместимость)

- `--oracle-texts-format auto`  
  Сначала пытается загрузить новую структуру (`activation/`, `service/`, `bridge/`, `answers/`, `closure/`), иначе использует legacy `.txt`.
- `--oracle-texts-format v2`  
  Только новый формат.
- `--oracle-texts-format legacy`  
  Только старые `.txt`.
- `--oracle-texts-legacy-compat`  
  В режимах `auto`/`v2` дополнительно подмешивает legacy `.txt` как fallback.
