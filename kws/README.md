# KWS Training Prep (Russian Archetypes)

## 1) Mapping File

Edit `kws/archetypes_map.json` if you want to change words per archetype.

## 2) Install Python Dependencies

```bash
python -m venv .venv
.venv\Scripts\activate
pip install huggingface_hub
```

## 3) Check Coverage in MSWC (ru)

Quick scan:

```bash
python kws/scripts/check_mswc_ru_coverage.py --map kws/archetypes_map.json
```

More complete scan (train + validation + test):

```bash
python kws/scripts/check_mswc_ru_coverage.py --map kws/archetypes_map.json --splits train,validation,test
```

Notes:
- Exit code `0`: all mapped words were found.
- Exit code `1`: some mapped words have `0` matches and should be replaced or collected manually.
- Exit code `2`: missing Python dependency or download error.

## 4) What to do with results

- If a word has enough data, keep it in map.
- If a word has low/zero count, replace with a synonym present in MSWC.
- Keep low-confidence handling deterministic (`unknown`/service prompt), without random archetype fallback.

## 5) Build Intent Manifest

Generate labeled samples for your mapped intents:

```bash
python kws/scripts/build_mswc_intent_manifest.py --map kws/archetypes_map.json --valid-only
```

Outputs:
- `kws/manifests/mswc_intent_manifest.csv`
- `kws/manifests/mswc_intent_stats.json`

## 6) Build Balanced Training Manifest (intents + unknown + silence)

```bash
python kws/scripts/build_balanced_kws_manifest.py \
  --map kws/archetypes_map.json \
  --intent-manifest kws/manifests/mswc_intent_manifest.csv \
  --valid-only \
  --unknown-ratio 1.0 \
  --silence-ratio 0.2
```

Outputs:
- `kws/manifests/kws_balanced_manifest.csv`
- `kws/manifests/kws_balanced_stats.json`

Notes:
- `unknown` is sampled from non-mapped words in MSWC.
- `silence` is emitted as `synthetic_silence` rows (to be generated in training pipeline).

## 7) Download clips used by manifest

Download only files referenced by balanced manifest:

```bash
python kws/scripts/download_mswc_clips_for_manifest.py \
  --manifest kws/manifests/kws_balanced_manifest.csv \
  --out-root D:/mswc_ru_clips
```

Tip:
- MSWC stores clips in shard archives (`data/<format>/<lang>/<split>/audio/*.tar.gz`).
- Script downloads only required shards and extracts only files from your manifest.
- If some files fail due network/Hub issues, rerun the same command; existing files are skipped.

Expected layout:

```text
<clips_root>/<word>/<file>.opus
```

Example:
`D:/mswc_ru_clips/решение/common_voice_ru_12345678.opus`

## 8) Train KWS model

```bash
pip install torch torchaudio

python kws/scripts/train_kws.py \
  --manifest kws/manifests/kws_balanced_manifest.csv \
  --clips-root D:/mswc_ru_clips \
  --out-dir kws/runs/kws_ru_v1 \
  --epochs 20 \
  --batch-size 64 \
  --workers 0
```

Outputs:
- `kws/runs/kws_ru_v1/kws_best.pt`
- `kws/runs/kws_ru_v1/labels.json`
- `kws/runs/kws_ru_v1/train_report.json`

Recommended anti-collapse options (when model predicts mostly `unknown`):

```bash
python kws/scripts/train_kws.py \
  --manifest kws/manifests_v2/kws_balanced_manifest.csv \
  --clips-root D:/mswc_ru_clips \
  --out-dir kws/runs/kws_ru_v2 \
  --epochs 25 \
  --batch-size 64 \
  --workers 0 \
  --balanced-sampler \
  --class-weighting \
  --unknown-loss-scale 0.25 \
  --silence-loss-scale 0.6
```

## 9) Evaluate per-class metrics (confusion matrix)

```bash
python kws/scripts/eval_kws.py \
  --manifest kws/manifests/kws_balanced_manifest.csv \
  --clips-root D:/mswc_ru_clips \
  --checkpoint kws/runs/kws_ru_v1/kws_best.pt \
  --run-dir kws/runs/kws_ru_v1 \
  --split test \
  --workers 0
```

Outputs:
- `kws/runs/kws_ru_v1/eval_test.json`
- `kws/runs/kws_ru_v1/confusion_test.csv`

## 10) Train Reject Model (known vs unknown)

```bash
python kws/scripts/train_reject.py \
  --manifest kws/manifests_v2/kws_balanced_manifest.csv \
  --clips-root D:/mswc_ru_clips \
  --out-dir kws/runs/reject_ru_v1 \
  --epochs 20 \
  --batch-size 64 \
  --workers 0
```

Outputs:
- `kws/runs/reject_ru_v1/reject_best.pt`
- `kws/runs/reject_ru_v1/reject_report.json`

## 11) Evaluate Two-Stage Pipeline (reject -> intent)

```bash
python kws/scripts/eval_two_stage.py \
  --manifest kws/manifests_v2/kws_balanced_manifest.csv \
  --clips-root D:/mswc_ru_clips \
  --reject-checkpoint kws/runs/reject_ru_v1/reject_best.pt \
  --intent-checkpoint kws/runs/kws_ru_v6/kws_best.pt \
  --run-dir kws/runs/two_stage_ru_v1 \
  --split test \
  --workers 0
```

Outputs:
- `kws/runs/two_stage_ru_v1/eval_two_stage_test.json`
- `kws/runs/two_stage_ru_v1/confusion_two_stage_test.csv`

## 12) (Recommended) Word-Level Stage-2 + intent mapping

Build word-level stage-2 manifest:

```bash
python kws/scripts/build_word_intent_manifest.py \
  --intent-manifest kws/manifests/mswc_intent_manifest.csv \
  --out-dir kws/manifests_word_v2 \
  --min-train-per-word 30 \
  --min-val-per-word 4 \
  --min-test-per-word 4 \
  --target-train-per-word 60 \
  --target-val-per-word 8 \
  --target-test-per-word 8 \
  --max-words-per-intent 2 \
  --silence-ratio 0.10
```

Download missing clips for this manifest:

```bash
python kws/scripts/download_mswc_clips_for_manifest.py \
  --manifest kws/manifests_word_v2/kws_word_manifest.csv \
  --out-root kws/data/mswc_ru_word_v2 \
  --format opus
```

Train stage-2 word model:

```bash
python kws/scripts/train_kws.py \
  --manifest kws/manifests_word_v2/kws_word_manifest.csv \
  --clips-root kws/data/mswc_ru_word_v2 \
  --out-dir kws/runs/kws_word_ru_v3 \
  --epochs 30 \
  --batch-size 64 \
  --workers 0 \
  --balanced-sampler \
  --width 40 \
  --lr 0.0005
```

Evaluate two-stage with word->intent mapping:

```bash
python kws/scripts/eval_two_stage.py \
  --manifest kws/manifests_v2/kws_balanced_manifest.csv \
  --clips-root D:/mswc_ru_clips \
  --reject-checkpoint kws/runs/reject_ru_v1/reject_best.pt \
  --intent-checkpoint kws/runs/kws_word_ru_v3/kws_best.pt \
  --stage2-word-map kws/manifests_word_v2/word_to_intent.json \
  --run-dir kws/runs/two_stage_ru_v2 \
  --split test \
  --workers 0
```

Tune reject threshold (recommended before firmware integration):

```bash
python kws/scripts/tune_two_stage_threshold.py \
  --manifest kws/manifests_v2/kws_balanced_manifest.csv \
  --clips-root D:/mswc_ru_clips \
  --reject-checkpoint kws/runs/reject_ru_v1/reject_best.pt \
  --intent-checkpoint kws/runs/kws_word_ru_v3/kws_best.pt \
  --stage2-word-map kws/manifests_word_v2/word_to_intent.json \
  --run-dir kws/runs/two_stage_ru_v2 \
  --split test \
  --workers 0 \
  --th-min 0.10 \
  --th-max 0.90 \
  --th-step 0.02 \
  --alpha-unknown-recall 0.30 \
  --min-known-recall 0.90
```

Current recommended threshold for this baseline:
- `reject_threshold = 0.38`

## Raspberry Pi WS Deploy (many files)

Use PowerShell deploy helper to pack/upload multiple paths with checksum verification:

```powershell
python -V
powershell -ExecutionPolicy Bypass -File kws/scripts/deploy_pi_ws.ps1 `
  -PiHost 192.168.43.242 `
  -PiUser prophet_master `
  -RemoteDir ~/orb_ws `
  -InstallDeps
```

Default synced paths:
- `kws/pi_ws`
- `kws/scripts/pi_mic_ws_server.py`

You can override for larger sync sets:

```powershell
powershell -ExecutionPolicy Bypass -File kws/scripts/deploy_pi_ws.ps1 `
  -IncludePaths @("kws/pi_ws","kws/scripts","kws/manifests_word_v5")
```

Run on Raspberry Pi:

```bash
cd ~/orb_ws
source .venv/bin/activate
python kws/scripts/pi_mic_ws_server.py \
  --host 0.0.0.0 \
  --port 8765 \
  --vosk-model ~/orb_ws/models/vosk-model-small-ru-0.22 \
  --tts-backend piper
```

Vosk model (once on Raspberry Pi):

```bash
mkdir -p ~/orb_ws/models
cd ~/orb_ws/models
wget https://alphacephei.com/vosk/models/vosk-model-small-ru-0.22.zip
unzip vosk-model-small-ru-0.22.zip
```
