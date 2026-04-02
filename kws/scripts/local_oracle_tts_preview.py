#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import os
import random
import time
import wave
from pathlib import Path
from typing import List, Tuple


def _bootstrap_path() -> None:
    kws_root = Path(__file__).resolve().parents[1]
    kws_root_str = str(kws_root)
    import sys

    if kws_root_str not in sys.path:
        sys.path.insert(0, kws_root_str)


_bootstrap_path()

from pi_ws.text_bank import DomainTextSet, OracleTextBank  # noqa: E402
from pi_ws.tts_synth import TtsConfig, synthesize_tts, validate_tts_backend  # noqa: E402


def _default_oracle_texts_dir() -> Path:
    return Path(__file__).resolve().parents[2] / "docs" / "texts"


def make_cfg(args: argparse.Namespace) -> TtsConfig:
    return TtsConfig(
        backend=str(args.tts_backend).strip().lower(),
        piper_bin=str(args.piper_bin).strip(),
        piper_model_dir=Path(args.piper_model_dir).expanduser(),
        piper_default_model=str(args.piper_default_model).strip(),
        piper_default_config=str(args.piper_default_config).strip(),
        piper_espeak_data_dir=Path(args.piper_espeak_data_dir).expanduser(),
        piper_length_scale=float(args.piper_length_scale),
        piper_noise_scale=float(args.piper_noise_scale),
        piper_noise_w=float(args.piper_noise_w),
        piper_sentence_silence_s=float(args.piper_sentence_silence_s),
        piper_pitch_scale=float(args.piper_pitch_scale),
        tts_tempo_scale=float(args.tts_tempo_scale),
        silero_repo_or_dir=str(args.silero_repo_or_dir).strip(),
        silero_language=str(args.silero_language).strip(),
        silero_speaker_model=str(args.silero_speaker_model).strip(),
        silero_speaker=str(args.silero_speaker).strip(),
        silero_sample_rate=int(args.silero_sample_rate),
        silero_device=str(args.silero_device).strip(),
        silero_put_accent=bool(args.silero_put_accent),
        silero_put_yo=bool(args.silero_put_yo),
        silero_num_threads=int(args.silero_num_threads),
        yandex_endpoint=str(args.yandex_endpoint).strip(),
        yandex_api_key=str(args.yandex_api_key).strip(),
        yandex_iam_token=str(args.yandex_iam_token).strip(),
        yandex_folder_id=str(args.yandex_folder_id).strip(),
        yandex_lang=str(args.yandex_lang).strip(),
        yandex_voice=str(args.yandex_voice).strip(),
        yandex_speed=float(args.yandex_speed),
        yandex_emotion=str(args.yandex_emotion).strip(),
        yandex_timeout_s=float(args.yandex_timeout_s),
        tts_echo_mix=float(args.tts_echo_mix),
        tts_echo_delay_ms=int(args.tts_echo_delay_ms),
        tts_echo_feedback=float(args.tts_echo_feedback),
        tts_reverb_mix=float(args.tts_reverb_mix),
        tts_reverb_room_scale=float(args.tts_reverb_room_scale),
        tts_reverb_damp=float(args.tts_reverb_damp),
        tts_tail_fade_ms=int(args.tts_tail_fade_ms),
        tts_tail_silence_ms=int(args.tts_tail_silence_ms),
        tone_hz=float(args.tts_tone_hz),
        tone_ms=int(args.tts_tone_ms),
        tone_gain=float(args.tts_tone_gain),
    )


def _domain_lines_full(data: DomainTextSet) -> List[str]:
    lines: List[str] = []
    lines.extend([x for x in data.greeting if x])
    lines.extend([x for x in data.understanding if x])
    lines.extend([x for x in data.prediction_positive if x])
    lines.extend([x for x in data.prediction_negative if x])
    lines.extend([x for x in data.prediction_neutral if x])
    lines.extend([x for x in data.farewell if x])
    return lines


def _domain_lines_script(bank: OracleTextBank, domain: str) -> List[str]:
    selection = bank.select_script(intent=domain, question_form="open", force_polarity="neutral")
    return [selection.phases.get("greeting", ""), selection.phases.get("understanding", ""), selection.phases.get("prediction", ""), selection.phases.get("farewell", "")]


def _pick_domain(bank: OracleTextBank, domain_arg: str, seed: int) -> str:
    if domain_arg and domain_arg.lower() != "random":
        return domain_arg.strip().lower()
    keys = sorted(k for k in bank.domains.keys() if k and "service" not in k)
    if not keys:
        return "future"
    rng = random.Random(seed if seed >= 0 else None)
    return rng.choice(keys)


def write_wav(path: Path, pcm_le16: bytes, sample_rate: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_le16)


async def run_preview(args: argparse.Namespace) -> int:
    texts_dir = Path(args.texts_dir).expanduser().resolve()
    bank = OracleTextBank.load_from_dir(texts_dir, seed=(args.seed if args.seed >= 0 else None))
    domain = _pick_domain(bank, args.domain, args.seed)

    cfg = make_cfg(args)
    backend_probe = await validate_tts_backend(cfg)

    data = bank.domains.get(domain)
    if data is None:
        print(f"ERROR unknown domain='{domain}'. available={sorted(bank.domains.keys())}")
        return 2

    if args.mode == "full":
        lines = _domain_lines_full(data)
    else:
        lines = _domain_lines_script(bank, domain)

    lines = [x.strip() for x in lines if x and x.strip()]
    if args.max_lines > 0:
        lines = lines[: args.max_lines]
    if not lines:
        print(f"ERROR no lines for domain={domain}")
        return 2

    print(
        f"local preview: domain={domain} mode={args.mode} lines={len(lines)} "
        f"backend={cfg.backend} probe={backend_probe} sample_rate={args.sample_rate}"
    )

    silence_samples = max(0, int(args.sample_rate * max(0, args.line_pause_ms) / 1000))
    silence = b"\x00\x00" * silence_samples

    start = time.perf_counter()
    audio = bytearray()
    total_audio_ms = 0
    for idx, line in enumerate(lines, start=1):
        res = await synthesize_tts(line, int(args.sample_rate), args.voice_hint, cfg)
        audio.extend(res.pcm_le16)
        total_audio_ms += int((len(res.pcm_le16) // 2) * 1000 / max(1, res.sample_rate))
        if idx < len(lines) and silence:
            audio.extend(silence)
            total_audio_ms += int(args.line_pause_ms)
        print(f"[{idx:03d}/{len(lines):03d}] chars={len(line)} bytes={len(res.pcm_le16)} model={res.model}")

    elapsed_ms = int((time.perf_counter() - start) * 1000.0)
    out_dir = Path(args.out_dir).expanduser()
    out_name = f"preview_{domain}_{cfg.backend}_{int(time.time())}.wav"
    out_path = out_dir / out_name
    write_wav(out_path, bytes(audio), int(args.sample_rate))

    rtf = float(elapsed_ms) / float(max(1, total_audio_ms))
    print(
        f"done: out={out_path} bytes={len(audio)} audio_ms={total_audio_ms} "
        f"synth_ms={elapsed_ms} rtf={rtf:.3f}"
    )
    return 0


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Local oracle TTS preview (random archetype, same TTS params).")
    p.add_argument("--texts-dir", default=str(_default_oracle_texts_dir()))
    p.add_argument("--domain", default="random", help="Domain name or 'random'")
    p.add_argument("--mode", choices=["full", "script"], default="full", help="'full' = all lines of archetype")
    p.add_argument("--seed", type=int, default=-1)
    p.add_argument("--max-lines", type=int, default=0, help="0 = all")
    p.add_argument("--line-pause-ms", type=int, default=220)
    p.add_argument("--sample-rate", type=int, default=int(os.getenv("ORB_WS_PREVIEW_SAMPLE_RATE", "24000")))
    p.add_argument("--voice-hint", default="")
    p.add_argument("--out-dir", default=os.getenv("ORB_WS_PREVIEW_OUT_DIR", "kws/captures/local_preview"))

    p.add_argument("--tts-backend", default=os.getenv("ORB_WS_TTS_BACKEND", "silero"), choices=["none", "piper", "silero", "yandex", "tone"])
    p.add_argument("--piper-bin", default=os.getenv("ORB_WS_PIPER_BIN", "~/orb_ws/piper/bin/piper"))
    p.add_argument("--piper-model-dir", default=os.getenv("ORB_WS_PIPER_MODEL_DIR", "~/orb_ws/models/piper"))
    p.add_argument("--piper-default-model", default=os.getenv("ORB_WS_PIPER_DEFAULT_MODEL", "ru_RU-ruslan-medium.onnx"))
    p.add_argument("--piper-default-config", default=os.getenv("ORB_WS_PIPER_DEFAULT_CONFIG", ""))
    p.add_argument("--piper-espeak-data-dir", default=os.getenv("ORB_WS_PIPER_ESPEAK_DATA_DIR", "/usr/lib/aarch64-linux-gnu/espeak-ng-data"))
    p.add_argument("--piper-length-scale", type=float, default=float(os.getenv("ORB_WS_PIPER_LENGTH_SCALE", "1.20")))
    p.add_argument("--piper-noise-scale", type=float, default=float(os.getenv("ORB_WS_PIPER_NOISE_SCALE", "0.28")))
    p.add_argument("--piper-noise-w", type=float, default=float(os.getenv("ORB_WS_PIPER_NOISE_W", "0.35")))
    p.add_argument("--piper-sentence-silence-s", type=float, default=float(os.getenv("ORB_WS_PIPER_SENTENCE_SILENCE_S", "0.34")))
    p.add_argument("--piper-pitch-scale", type=float, default=float(os.getenv("ORB_WS_PIPER_PITCH_SCALE", "0.95")))
    p.add_argument("--tts-tempo-scale", type=float, default=float(os.getenv("ORB_WS_TTS_TEMPO_SCALE", "1.00")))

    p.add_argument("--silero-repo-or-dir", default=os.getenv("ORB_WS_SILERO_REPO_OR_DIR", "snakers4/silero-models"))
    p.add_argument("--silero-language", default=os.getenv("ORB_WS_SILERO_LANGUAGE", "ru"))
    p.add_argument("--silero-speaker-model", default=os.getenv("ORB_WS_SILERO_SPEAKER_MODEL", "v4_ru"))
    p.add_argument("--silero-speaker", default=os.getenv("ORB_WS_SILERO_SPEAKER", "xenia"))
    p.add_argument("--silero-sample-rate", type=int, default=int(os.getenv("ORB_WS_SILERO_SAMPLE_RATE", "24000")))
    p.add_argument("--silero-device", default=os.getenv("ORB_WS_SILERO_DEVICE", "cpu"))
    p.add_argument("--silero-put-accent", action="store_true", default=(os.getenv("ORB_WS_SILERO_PUT_ACCENT", "1") != "0"))
    p.add_argument("--silero-no-put-accent", dest="silero_put_accent", action="store_false")
    p.add_argument("--silero-put-yo", action="store_true", default=(os.getenv("ORB_WS_SILERO_PUT_YO", "1") != "0"))
    p.add_argument("--silero-no-put-yo", dest="silero_put_yo", action="store_false")
    p.add_argument("--silero-num-threads", type=int, default=int(os.getenv("ORB_WS_SILERO_NUM_THREADS", "4")))

    p.add_argument("--yandex-endpoint", default=os.getenv("ORB_WS_YANDEX_ENDPOINT", "https://tts.api.cloud.yandex.net/speech/v1/tts:synthesize"))
    p.add_argument("--yandex-api-key", default=os.getenv("ORB_WS_YANDEX_API_KEY", ""))
    p.add_argument("--yandex-iam-token", default=os.getenv("ORB_WS_YANDEX_IAM_TOKEN", ""))
    p.add_argument("--yandex-folder-id", default=os.getenv("ORB_WS_YANDEX_FOLDER_ID", ""))
    p.add_argument("--yandex-lang", default=os.getenv("ORB_WS_YANDEX_LANG", "ru-RU"))
    p.add_argument("--yandex-voice", default=os.getenv("ORB_WS_YANDEX_VOICE", "ermil"))
    p.add_argument("--yandex-speed", type=float, default=float(os.getenv("ORB_WS_YANDEX_SPEED", "1.0")))
    p.add_argument("--yandex-emotion", default=os.getenv("ORB_WS_YANDEX_EMOTION", ""))
    p.add_argument("--yandex-timeout-s", type=float, default=float(os.getenv("ORB_WS_YANDEX_TIMEOUT_S", "20.0")))

    p.add_argument("--tts-echo-mix", type=float, default=float(os.getenv("ORB_WS_TTS_ECHO_MIX", "0.16")))
    p.add_argument("--tts-echo-delay-ms", type=int, default=int(os.getenv("ORB_WS_TTS_ECHO_DELAY_MS", "185")))
    p.add_argument("--tts-echo-feedback", type=float, default=float(os.getenv("ORB_WS_TTS_ECHO_FEEDBACK", "0.30")))
    p.add_argument("--tts-reverb-mix", type=float, default=float(os.getenv("ORB_WS_TTS_REVERB_MIX", "0.00")))
    p.add_argument("--tts-reverb-room-scale", type=float, default=float(os.getenv("ORB_WS_TTS_REVERB_ROOM_SCALE", "1.0")))
    p.add_argument("--tts-reverb-damp", type=float, default=float(os.getenv("ORB_WS_TTS_REVERB_DAMP", "0.30")))
    p.add_argument("--tts-tail-fade-ms", type=int, default=int(os.getenv("ORB_WS_TTS_TAIL_FADE_MS", "12")))
    p.add_argument("--tts-tail-silence-ms", type=int, default=int(os.getenv("ORB_WS_TTS_TAIL_SILENCE_MS", "140")))
    p.add_argument("--tts-tone-hz", type=float, default=float(os.getenv("ORB_WS_TTS_TONE_HZ", "280.0")))
    p.add_argument("--tts-tone-ms", type=int, default=int(os.getenv("ORB_WS_TTS_TONE_MS", "1400")))
    p.add_argument("--tts-tone-gain", type=float, default=float(os.getenv("ORB_WS_TTS_TONE_GAIN", "0.35")))

    return p.parse_args()


def main() -> int:
    return asyncio.run(run_preview(parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())

