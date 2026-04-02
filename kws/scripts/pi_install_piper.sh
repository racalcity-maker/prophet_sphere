#!/usr/bin/env bash
set -euo pipefail

PIPER_VERSION="${PIPER_VERSION:-v1.2.0}"
ORB_WS_DIR="${ORB_WS_DIR:-$HOME/orb_ws}"
PIPER_ROOT="${PIPER_ROOT:-$ORB_WS_DIR/piper}"
PIPER_BIN_DIR="${PIPER_BIN_DIR:-$PIPER_ROOT/bin}"
PIPER_MODEL_DIR="${PIPER_MODEL_DIR:-$ORB_WS_DIR/models/piper}"
VOICE_KEY="${VOICE_KEY:-ru_RU-ruslan-medium}"
VOICE_LANG="${VOICE_LANG:-ru}"
VOICE_LOCALE="${VOICE_LOCALE:-ru_RU}"
VOICE_TAIL="${VOICE_KEY#${VOICE_LOCALE}-}"
VOICE_SPEAKER="${VOICE_SPEAKER:-${VOICE_TAIL%-*}}"
VOICE_QUALITY="${VOICE_QUALITY:-${VOICE_TAIL##*-}}"
if [ -z "$VOICE_SPEAKER" ] || [ "$VOICE_SPEAKER" = "$VOICE_TAIL" ]; then
  VOICE_SPEAKER="ruslan"
fi
if [ -z "$VOICE_QUALITY" ]; then
  VOICE_QUALITY="medium"
fi

mkdir -p "$PIPER_BIN_DIR" "$PIPER_MODEL_DIR"

ARCH="$(uname -m)"
case "$ARCH" in
  aarch64|arm64)
    PIPER_ASSET="piper_arm64.tar.gz"
    ;;
  armv7l|armv7*)
    PIPER_ASSET="piper_armv7.tar.gz"
    ;;
  x86_64|amd64)
    PIPER_ASSET="piper_amd64.tar.gz"
    ;;
  *)
    echo "Unsupported architecture: $ARCH"
    exit 1
    ;;
esac

PIPER_URL="https://github.com/rhasspy/piper/releases/download/${PIPER_VERSION}/${PIPER_ASSET}"
VOICE_ONNX_URL="https://huggingface.co/rhasspy/piper-voices/resolve/main/${VOICE_LANG}/${VOICE_LOCALE}/${VOICE_SPEAKER}/${VOICE_QUALITY}/${VOICE_KEY}.onnx"
VOICE_JSON_URL="https://huggingface.co/rhasspy/piper-voices/resolve/main/${VOICE_LANG}/${VOICE_LOCALE}/${VOICE_SPEAKER}/${VOICE_QUALITY}/${VOICE_KEY}.onnx.json"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "[1/4] Download Piper binary: $PIPER_URL"
curl -fL "$PIPER_URL" -o "$TMP_DIR/piper.tar.gz"
tar -xzf "$TMP_DIR/piper.tar.gz" -C "$TMP_DIR"

if [ ! -x "$TMP_DIR/piper/piper" ]; then
  echo "Piper binary not found in archive"
  exit 1
fi

echo "[2/4] Install Piper to $PIPER_BIN_DIR"
cp -f "$TMP_DIR/piper/piper" "$PIPER_BIN_DIR/piper"
chmod +x "$PIPER_BIN_DIR/piper"
if [ -d "$TMP_DIR/piper/lib" ]; then
  mkdir -p "$PIPER_ROOT/lib"
  cp -Rf "$TMP_DIR/piper/lib/." "$PIPER_ROOT/lib/"
fi
if [ -d "$TMP_DIR/piper/lib64" ]; then
  mkdir -p "$PIPER_ROOT/lib64"
  cp -Rf "$TMP_DIR/piper/lib64/." "$PIPER_ROOT/lib64/"
fi
if [ -d "$TMP_DIR/piper" ]; then
  # Some builds place shared libs in architecture subfolders.
  while IFS= read -r -d '' libf; do
    rel="${libf#"$TMP_DIR/piper/"}"
    dstdir="$PIPER_ROOT/$(dirname "$rel")"
    mkdir -p "$dstdir"
    cp -f "$libf" "$dstdir/"
  done < <(find "$TMP_DIR/piper" -type f \( -name '*.so' -o -name '*.so.*' \) -print0)
fi
# Locate espeak-ng-data in archive (layout differs by build).
ESPEAK_SRC_DIR=""
while IFS= read -r d; do
  ESPEAK_SRC_DIR="$d"
  break
done < <(find "$TMP_DIR/piper" -type d -name 'espeak-ng-data' | sort)
if [ -n "$ESPEAK_SRC_DIR" ]; then
  mkdir -p "$PIPER_ROOT/espeak-ng-data"
  cp -Rf "$ESPEAK_SRC_DIR/." "$PIPER_ROOT/espeak-ng-data/"
fi

echo "[3/4] Download voice: $VOICE_KEY"
curl -fL "$VOICE_ONNX_URL" -o "$PIPER_MODEL_DIR/${VOICE_KEY}.onnx"
curl -fL "$VOICE_JSON_URL" -o "$PIPER_MODEL_DIR/${VOICE_KEY}.onnx.json"

echo "[4/4] Validate Piper runtime"
PIPER_LD_PATHS=""
while IFS= read -r d; do
  if [ -n "$PIPER_LD_PATHS" ]; then
    PIPER_LD_PATHS="${PIPER_LD_PATHS}:$d"
  else
    PIPER_LD_PATHS="$d"
  fi
done < <(find "$PIPER_ROOT" -type f \( -name 'libpiper_phonemize.so*' -o -name 'libespeak-ng.so*' -o -name 'libonnxruntime.so*' \) -printf '%h\n' | sort -u)
if [ -z "$PIPER_LD_PATHS" ]; then
  PIPER_LD_PATHS="$PIPER_ROOT/lib:$PIPER_ROOT/lib64"
fi
export LD_LIBRARY_PATH="$PIPER_LD_PATHS:${LD_LIBRARY_PATH:-}"
ESPEAK_DATA_DIR=""
if [ -d "$PIPER_ROOT/espeak-ng-data" ]; then
  ESPEAK_DATA_DIR="$PIPER_ROOT/espeak-ng-data"
elif [ -d "/usr/share/espeak-ng-data" ]; then
  ESPEAK_DATA_DIR="/usr/share/espeak-ng-data"
elif [ -d "/usr/lib/x86_64-linux-gnu/espeak-ng-data" ]; then
  ESPEAK_DATA_DIR="/usr/lib/x86_64-linux-gnu/espeak-ng-data"
elif [ -d "/usr/lib/aarch64-linux-gnu/espeak-ng-data" ]; then
  ESPEAK_DATA_DIR="/usr/lib/aarch64-linux-gnu/espeak-ng-data"
fi

if [ -z "$ESPEAK_DATA_DIR" ]; then
  echo "ERROR: espeak-ng-data not found. Install system package first:" >&2
  echo "  sudo apt update && sudo apt install -y espeak-ng-data libespeak-ng1" >&2
  exit 1
fi

echo "привет мир" | "$PIPER_BIN_DIR/piper" --model "$PIPER_MODEL_DIR/${VOICE_KEY}.onnx" --espeak_data "$ESPEAK_DATA_DIR" --output_raw >/dev/null

cat <<EOF
OK
PIPER_BIN=$PIPER_BIN_DIR/piper
PIPER_MODEL=$PIPER_MODEL_DIR/${VOICE_KEY}.onnx
Run server with:
  ORB_WS_PIPER_BIN=$PIPER_BIN_DIR/piper \\
  ORB_WS_PIPER_MODEL_DIR=$PIPER_MODEL_DIR \\
  ORB_WS_PIPER_DEFAULT_MODEL=${VOICE_KEY}.onnx \\
  ORB_WS_PIPER_ESPEAK_DATA_DIR=$ESPEAK_DATA_DIR \\
  ORB_WS_TTS_BACKEND=piper \\
  ORB_WS_TTS_FX_PRESET=mystic \\
  ORB_WS_PIPER_LENGTH_SCALE=1.38 \\
  ORB_WS_PIPER_SENTENCE_SILENCE_S=0.34 \\
  ORB_WS_PIPER_PITCH_SCALE=0.80 \\
  ORB_WS_TTS_TONE_HZ=280 \\
  LD_LIBRARY_PATH=$PIPER_LD_PATHS:\$LD_LIBRARY_PATH \\
  python kws/scripts/pi_mic_ws_server.py --host 0.0.0.0 --port 8765 --vosk-model ~/orb_ws/models/vosk-model-small-ru-0.22 --piper-espeak-data-dir $ESPEAK_DATA_DIR
EOF
