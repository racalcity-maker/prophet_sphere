from __future__ import annotations

import asyncio
import json
import math
import os
import re
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

import numpy as np


@dataclass(frozen=True)
class TtsConfig:
    backend: str
    piper_bin: str
    piper_model_dir: Path
    piper_default_model: str
    piper_default_config: str
    piper_espeak_data_dir: Path
    piper_length_scale: float
    piper_noise_scale: float
    piper_noise_w: float
    piper_sentence_silence_s: float
    piper_pitch_scale: float
    tts_tempo_scale: float
    silero_repo_or_dir: str
    silero_language: str
    silero_speaker_model: str
    silero_speaker: str
    silero_sample_rate: int
    silero_device: str
    silero_put_accent: bool
    silero_put_yo: bool
    silero_num_threads: int
    yandex_endpoint: str
    yandex_api_key: str
    yandex_iam_token: str
    yandex_folder_id: str
    yandex_lang: str
    yandex_voice: str
    yandex_speed: float
    yandex_emotion: str
    yandex_timeout_s: float
    tts_echo_mix: float
    tts_echo_delay_ms: int
    tts_echo_feedback: float
    tts_reverb_mix: float
    tts_reverb_room_scale: float
    tts_reverb_damp: float
    tts_tail_fade_ms: int
    tts_tail_silence_ms: int
    tone_hz: float
    tone_ms: int
    tone_gain: float


@dataclass(frozen=True)
class TtsResult:
    pcm_le16: bytes
    sample_rate: int
    backend: str
    model: str


_SILERO_CACHE: Dict[Tuple[str, str, str, str], Any] = {}


def _text_preview(text: str, limit: int = 180) -> str:
    s = (text or "").replace("\r", "\\r").replace("\n", "\\n")
    if len(s) <= limit:
        return s
    return s[:limit] + "..."


def _clamp_gain(gain: float) -> float:
    if not math.isfinite(gain):
        return 0.5
    return max(0.0, min(1.0, gain))


def _resample_linear_i16(samples: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    if samples.size == 0:
        return samples.astype(np.int16, copy=False)
    if src_rate <= 0 or dst_rate <= 0 or src_rate == dst_rate:
        return samples.astype(np.int16, copy=False)

    src = samples.astype(np.float32)
    src_n = int(src.size)
    dst_n = max(1, int(round(src_n * float(dst_rate) / float(src_rate))))

    if src_n == 1:
        out = np.full((dst_n,), src[0], dtype=np.float32)
    else:
        x_old = np.linspace(0.0, 1.0, num=src_n, endpoint=False, dtype=np.float32)
        x_new = np.linspace(0.0, 1.0, num=dst_n, endpoint=False, dtype=np.float32)
        out = np.interp(x_new, x_old, src).astype(np.float32)

    out = np.clip(out, -32768.0, 32767.0)
    return out.astype(np.int16)


def _apply_pitch_rate_scale_i16(samples: np.ndarray, pitch_scale: float) -> np.ndarray:
    """
    Simple pitch-rate coupling transform:
    pitch_scale < 1.0 -> lower pitch and slower speech
    pitch_scale > 1.0 -> higher pitch and faster speech
    """
    if samples.size == 0:
        return samples.astype(np.int16, copy=False)
    if not math.isfinite(pitch_scale):
        return samples.astype(np.int16, copy=False)

    scale = max(0.7, min(1.3, float(pitch_scale)))
    if abs(scale - 1.0) < 0.01:
        return samples.astype(np.int16, copy=False)

    pseudo_src_rate = max(1, int(round(1000.0 * scale)))
    pseudo_dst_rate = 1000
    return _resample_linear_i16(samples, pseudo_src_rate, pseudo_dst_rate)


def _apply_tempo_scale_i16(samples: np.ndarray, tempo_scale: float) -> np.ndarray:
    """
    Tempo/rate transform:
    tempo_scale < 1.0 -> slower speech
    tempo_scale > 1.0 -> faster speech
    """
    if samples.size == 0:
        return samples.astype(np.int16, copy=False)
    if not math.isfinite(tempo_scale):
        return samples.astype(np.int16, copy=False)

    scale = max(0.65, min(1.40, float(tempo_scale)))
    if abs(scale - 1.0) < 0.01:
        return samples.astype(np.int16, copy=False)

    pseudo_src_rate = max(1, int(round(1000.0 * scale)))
    pseudo_dst_rate = 1000
    return _resample_linear_i16(samples, pseudo_src_rate, pseudo_dst_rate)


def _postprocess_tail(samples: np.ndarray, sample_rate: int, fade_ms: int, silence_ms: int) -> np.ndarray:
    if samples.size == 0 or sample_rate <= 0:
        return samples.astype(np.int16, copy=False)

    out = samples.astype(np.int16, copy=True)
    # Short fade-in to suppress click at segment start.
    fade_in_n = max(0, int(sample_rate * 12 / 1000))
    fade_n = max(0, int(sample_rate * max(0, int(fade_ms)) / 1000))
    silence_n = max(0, int(sample_rate * max(0, int(silence_ms)) / 1000))

    if fade_in_n > 1:
        n = min(fade_in_n, out.size)
        ramp = np.linspace(0.0, 1.0, num=n, endpoint=True, dtype=np.float32)
        head = out[:n].astype(np.float32) * ramp
        out[:n] = np.clip(head, -32768.0, 32767.0).astype(np.int16)

    if fade_n > 1:
        n = min(fade_n, out.size)
        ramp = np.linspace(1.0, 0.0, num=n, endpoint=True, dtype=np.float32)
        tail = out[-n:].astype(np.float32) * ramp
        out[-n:] = np.clip(tail, -32768.0, 32767.0).astype(np.int16)
        # Ensure exact zero at segment end to avoid click on stitch/pause boundaries.
        out[-1] = np.int16(0)

    if silence_n > 0:
        out = np.concatenate((out, np.zeros((silence_n,), dtype=np.int16)))

    return out


def _apply_echo_i16(
    samples: np.ndarray,
    sample_rate: int,
    *,
    mix: float,
    delay_ms: int,
    feedback: float,
) -> np.ndarray:
    if samples.size == 0 or sample_rate <= 0:
        return samples.astype(np.int16, copy=False)
    mix = max(0.0, min(1.0, float(mix)))
    if mix <= 0.0:
        return samples.astype(np.int16, copy=False)

    delay_n = max(1, int(sample_rate * max(10, int(delay_ms)) / 1000))
    fb = max(0.05, min(0.95, float(feedback)))

    dry = samples.astype(np.float32)
    wet = dry.copy()
    repeats = max(1, min(8, int(math.ceil(1.0 / max(1e-3, 1.0 - fb)))))
    for r in range(1, repeats + 1):
        d = delay_n * r
        if d >= dry.size:
            break
        g = fb ** r
        wet[d:] += dry[:-d] * g

    out = dry * (1.0 - mix) + wet * mix
    return np.clip(out, -32768.0, 32767.0).astype(np.int16)


def _apply_reverb_i16(
    samples: np.ndarray,
    sample_rate: int,
    *,
    mix: float,
    room_scale: float,
    damp: float,
) -> np.ndarray:
    if samples.size == 0 or sample_rate <= 0:
        return samples.astype(np.int16, copy=False)
    mix = max(0.0, min(1.0, float(mix)))
    if mix <= 0.0:
        return samples.astype(np.int16, copy=False)

    room = max(0.2, min(1.6, float(room_scale)))
    damp = max(0.0, min(0.95, float(damp)))

    dry = samples.astype(np.float32)
    wet = dry * 0.55

    base_delays_ms = (15.0, 23.0, 31.0, 41.0, 53.0)
    base_gains = (0.46, 0.36, 0.27, 0.20, 0.14)
    for d_ms, g in zip(base_delays_ms, base_gains):
        d = max(1, int(sample_rate * (d_ms * room) / 1000.0))
        if d >= dry.size:
            continue
        wet[d:] += dry[:-d] * g

    # Light diffusion + damping (single-pole low-pass on wet path).
    if wet.size > 1:
        lp = wet.copy()
        a = 1.0 - damp
        for i in range(1, lp.size):
            lp[i] = a * lp[i] + damp * lp[i - 1]
        wet = lp

    out = dry * (1.0 - mix) + wet * mix
    return np.clip(out, -32768.0, 32767.0).astype(np.int16)


def _resolve_model_path(cfg: TtsConfig, voice_hint: str) -> Optional[Path]:
    hint = (voice_hint or "").strip()
    model_dir = cfg.piper_model_dir.expanduser().resolve()

    # 0) explicit configured default model has highest priority
    if cfg.piper_default_model:
        d = (model_dir / cfg.piper_default_model).resolve()
        if d.exists() and d.is_file():
            return d
        # Do not silently fallback to another voice if default model is explicitly requested.
        return None

    # 1) direct file path in voice field
    if hint:
        p = Path(hint).expanduser()
        if p.exists() and p.is_file():
            return p.resolve()
        if p.suffix == ".onnx":
            q = (model_dir / p.name).resolve()
            if q.exists() and q.is_file():
                return q

    # 2) match by stem containing voice hint
    if hint and model_dir.exists():
        lower = hint.lower()
        for cand in sorted(model_dir.glob("*.onnx")):
            if lower in cand.stem.lower():
                return cand.resolve()

    # 3) first model in directory
    if model_dir.exists():
        models = sorted(model_dir.glob("*.onnx"))
        if models:
            return models[0].resolve()

    return None


def _resolve_model_sample_rate(model_path: Path, cfg: TtsConfig) -> int:
    candidates = []
    cfg_name = cfg.piper_default_config.strip()
    if cfg_name:
        candidates.append((model_path.parent / cfg_name).resolve())
    candidates.append(Path(str(model_path) + ".json"))
    candidates.append(model_path.with_suffix(model_path.suffix + ".json"))
    candidates.append(model_path.with_suffix(".onnx.json"))

    for c in candidates:
        if not c.exists() or not c.is_file():
            continue
        try:
            data = json.loads(c.read_text(encoding="utf-8"))
            audio = data.get("audio", {})
            sr = int(audio.get("sample_rate", 0))
            if sr > 0:
                return sr
        except Exception:
            continue
    # Most ru_RU piper models are 22050Hz.
    return 22050


async def _synthesize_piper(text: str, target_rate: int, voice_hint: str, cfg: TtsConfig) -> TtsResult:
    model_path = _resolve_model_path(cfg, voice_hint)
    if model_path is None:
        model_dir = cfg.piper_model_dir.expanduser().resolve()
        if cfg.piper_default_model:
            raise RuntimeError(
                f"piper default model not found: {model_dir / cfg.piper_default_model}"
            )
        raise RuntimeError("piper model not found")

    piper_bin = cfg.piper_bin
    if any(sep in cfg.piper_bin for sep in ("/", "\\")) or cfg.piper_bin.startswith("~") or cfg.piper_bin.startswith("."):
        piper_path = Path(cfg.piper_bin).expanduser().resolve()
        if not piper_path.exists() or not piper_path.is_file():
            raise RuntimeError(f"piper binary not found: {piper_path}")
        piper_bin = str(piper_path)

    espeak_data_dir = cfg.piper_espeak_data_dir.expanduser().resolve()

    env = os.environ.copy()
    cmd = [
        piper_bin,
        "--model",
        str(model_path),
        "--output_raw",
        "--length_scale",
        f"{max(0.35, min(2.5, float(cfg.piper_length_scale))):.3f}",
        "--noise_scale",
        f"{max(0.0, min(2.0, float(cfg.piper_noise_scale))):.3f}",
        "--noise_w",
        f"{max(0.0, min(2.0, float(cfg.piper_noise_w))):.3f}",
        "--sentence_silence",
        f"{max(0.0, min(2.0, float(cfg.piper_sentence_silence_s))):.3f}",
    ]
    if espeak_data_dir.exists() and espeak_data_dir.is_dir():
        cmd.extend(["--espeak_data", str(espeak_data_dir)])
    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        env=env,
    )

    stdin_payload = (text.strip() + "\n").encode("utf-8")
    raw_out, raw_err = await proc.communicate(stdin_payload)
    if proc.returncode != 0:
        err_text = raw_err.decode("utf-8", errors="ignore").strip()
        raise RuntimeError(f"piper failed rc={proc.returncode}: {err_text or 'unknown error'}")
    if not raw_out:
        raise RuntimeError("piper returned empty audio")

    src_rate = _resolve_model_sample_rate(model_path, cfg)
    src_samples = np.frombuffer(raw_out, dtype="<i2")
    out_samples = _resample_linear_i16(src_samples, src_rate, target_rate)
    out_samples = _apply_pitch_rate_scale_i16(out_samples, cfg.piper_pitch_scale)
    out_samples = _apply_tempo_scale_i16(out_samples, cfg.tts_tempo_scale)
    out_samples = _apply_echo_i16(
        out_samples,
        target_rate,
        mix=cfg.tts_echo_mix,
        delay_ms=cfg.tts_echo_delay_ms,
        feedback=cfg.tts_echo_feedback,
    )
    out_samples = _apply_reverb_i16(
        out_samples,
        target_rate,
        mix=cfg.tts_reverb_mix,
        room_scale=cfg.tts_reverb_room_scale,
        damp=cfg.tts_reverb_damp,
    )
    out_samples = _postprocess_tail(
        out_samples,
        target_rate,
        fade_ms=cfg.tts_tail_fade_ms,
        silence_ms=cfg.tts_tail_silence_ms,
    )
    return TtsResult(
        pcm_le16=out_samples.astype("<i2", copy=False).tobytes(),
        sample_rate=target_rate,
        backend="piper",
        model=str(model_path),
    )


def _normalize_text_for_silero(text: str) -> str:
    s = (text or "").strip()
    if not s:
        return ""

    # Normalize punctuation variants and remove stress marker leftovers.
    s = (
        s.replace("…", ". ")
        .replace("—", ", ")
        .replace("–", ", ")
        .replace("«", "\"")
        .replace("»", "\"")
        .replace("+", "")
    )

    # Keep characters Silero typically handles well.
    s = re.sub(r"[^0-9A-Za-zА-Яа-яЁё.,!?;:()\"' \t\r\n-]", " ", s)
    s = re.sub(r"[ \t]+", " ", s)
    s = re.sub(r"\s*\n\s*", " ", s).strip()
    s = re.sub(r"([.,!?;:])\1+", r"\1", s)
    s = re.sub(r"\s+([.,!?;:])", r"\1", s)

    if s and s[-1] not in ".!?":
        s += "."
    return s


def _silero_text_diag(original_text: str, normalized_text: str) -> str:
    orig = original_text or ""
    norm = normalized_text or ""
    replaced = []
    allowed = re.compile(r"[0-9A-Za-zА-Яа-яЁё.,!?;:()\"' \t\r\n-]")
    for ch in orig:
        if not allowed.fullmatch(ch):
            replaced.append(f"U+{ord(ch):04X}:{repr(ch)}")
    if len(replaced) > 24:
        replaced = replaced[:24] + ["..."]
    return (
        f"orig_len={len(orig)} norm_len={len(norm)} "
        f"orig='{_text_preview(orig)}' norm='{_text_preview(norm)}' "
        f"replaced=[{', '.join(replaced)}]"
    )


def _silero_apply_with_fallback(model: Any, kwargs: Dict[str, Any]) -> np.ndarray:
    original_text = str(kwargs.get("text", "") or "").strip()
    normalized_text = _normalize_text_for_silero(original_text)

    candidate_texts = []
    if original_text:
        candidate_texts.append(original_text)
    if normalized_text and normalized_text not in candidate_texts:
        candidate_texts.append(normalized_text)

    style_variants = [
        (bool(kwargs.get("put_accent", True)), bool(kwargs.get("put_yo", True))),
        (False, False),
    ]

    last_exc: Optional[Exception] = None
    for txt in candidate_texts:
        for put_accent, put_yo in style_variants:
            call_kwargs = dict(kwargs)
            call_kwargs["text"] = txt
            call_kwargs["put_accent"] = put_accent
            call_kwargs["put_yo"] = put_yo
            try:
                audio = model.apply_tts(**call_kwargs)
                arr = np.asarray(audio, dtype=np.float32).reshape(-1)
                if arr.size > 0:
                    return arr
            except ValueError as exc:
                last_exc = exc
                continue

    # Final fallback: split into simple sentences and synthesize piecewise.
    split_src = normalized_text or original_text
    parts = [p.strip() for p in re.split(r"[.!?;:]+", split_src) if p.strip()]
    if len(parts) > 1:
        sr = int(max(8000, min(48000, int(kwargs.get("sample_rate", 44100)))))
        pause = np.zeros((max(1, int(sr * 0.12)),), dtype=np.float32)
        chunks: list[np.ndarray] = []
        for i, part in enumerate(parts):
            piece = part if part.endswith((".", "!", "?")) else (part + ".")
            ok_piece = False
            for put_accent, put_yo in style_variants:
                call_kwargs = dict(kwargs)
                call_kwargs["text"] = piece
                call_kwargs["put_accent"] = put_accent
                call_kwargs["put_yo"] = put_yo
                try:
                    audio = model.apply_tts(**call_kwargs)
                    arr = np.asarray(audio, dtype=np.float32).reshape(-1)
                    if arr.size > 0:
                        chunks.append(arr)
                        if i + 1 < len(parts):
                            chunks.append(pause)
                        ok_piece = True
                        break
                except ValueError as exc:
                    last_exc = exc
                    continue
            if not ok_piece:
                break
        if chunks:
            merged = np.concatenate(chunks)
            if merged.size > 0:
                return merged

    raise RuntimeError(
        "silero rejected text after normalization; "
        + _silero_text_diag(original_text, normalized_text)
    ) from last_exc


def _load_silero_model(cfg: TtsConfig) -> Tuple[Any, str]:
    try:
        import torch  # type: ignore
    except Exception as exc:
        raise RuntimeError("silero backend requires torch (pip install torch)") from exc

    repo_or_dir = (cfg.silero_repo_or_dir or "snakers4/silero-models").strip()
    language = (cfg.silero_language or "ru").strip()
    speaker_model = (cfg.silero_speaker_model or "v4_ru").strip()
    device = (cfg.silero_device or "cpu").strip() or "cpu"
    cache_key = (repo_or_dir, language, speaker_model, device)

    cached = _SILERO_CACHE.get(cache_key)
    if cached is not None:
        return cached, speaker_model

    torch.set_num_threads(max(1, int(cfg.silero_num_threads)))
    model, _ = torch.hub.load(
        repo_or_dir=repo_or_dir,
        model="silero_tts",
        language=language,
        speaker=speaker_model,
        trust_repo=True,
    )
    try:
        model.to(device)
    except Exception:
        # keep CPU when requested device is unavailable
        pass
    _SILERO_CACHE[cache_key] = model
    return model, speaker_model


def _silero_synthesize_sync(text: str, target_rate: int, cfg: TtsConfig) -> TtsResult:
    model, speaker_model = _load_silero_model(cfg)
    speaker = (cfg.silero_speaker or "").strip()
    if not speaker:
        speakers = getattr(model, "speakers", None)
        if isinstance(speakers, (list, tuple)) and len(speakers) > 0:
            speaker = str(speakers[0])
    if not speaker:
        speaker = "xenia"

    normalized = _normalize_text_for_silero(text)
    if not normalized:
        raise RuntimeError("silero empty text after normalization")

    kwargs: Dict[str, Any] = {
        "text": normalized,
        "speaker": speaker,
        "sample_rate": int(max(8000, min(48000, cfg.silero_sample_rate))),
    }
    kwargs["put_accent"] = bool(cfg.silero_put_accent)
    kwargs["put_yo"] = bool(cfg.silero_put_yo)

    audio_np = _silero_apply_with_fallback(model, kwargs)
    if audio_np.size == 0:
        raise RuntimeError("silero returned empty audio")

    src_rate = int(max(8000, min(48000, cfg.silero_sample_rate)))
    src_i16 = np.clip(audio_np * 32767.0, -32768.0, 32767.0).astype(np.int16)
    out_samples = _resample_linear_i16(src_i16, src_rate, target_rate)
    out_samples = _apply_pitch_rate_scale_i16(out_samples, cfg.piper_pitch_scale)
    out_samples = _apply_tempo_scale_i16(out_samples, cfg.tts_tempo_scale)
    out_samples = _apply_echo_i16(
        out_samples,
        target_rate,
        mix=cfg.tts_echo_mix,
        delay_ms=cfg.tts_echo_delay_ms,
        feedback=cfg.tts_echo_feedback,
    )
    out_samples = _apply_reverb_i16(
        out_samples,
        target_rate,
        mix=cfg.tts_reverb_mix,
        room_scale=cfg.tts_reverb_room_scale,
        damp=cfg.tts_reverb_damp,
    )
    out_samples = _postprocess_tail(
        out_samples,
        target_rate,
        fade_ms=cfg.tts_tail_fade_ms,
        silence_ms=cfg.tts_tail_silence_ms,
    )
    return TtsResult(
        pcm_le16=out_samples.astype("<i2", copy=False).tobytes(),
        sample_rate=target_rate,
        backend="silero",
        model=f"{speaker_model}:{speaker}",
    )


def _silero_prewarm_sync(cfg: TtsConfig) -> None:
    """
    Run tiny inference once to avoid first-request latency spike on cold model.
    """
    try:
        warm_text = "warmup voice check ready"
        target_rate = int(max(8000, min(48000, cfg.silero_sample_rate)))
        _silero_synthesize_sync(warm_text, target_rate, cfg)
    except Exception:
        # Prewarm is best-effort; normal request path will still report real errors.
        return


async def _synthesize_silero(text: str, target_rate: int, cfg: TtsConfig) -> TtsResult:
    return await asyncio.to_thread(_silero_synthesize_sync, text, target_rate, cfg)


def _yandex_synthesize_sync(text: str, target_rate: int, cfg: TtsConfig) -> TtsResult:
    endpoint = (cfg.yandex_endpoint or "").strip()
    if not endpoint:
        raise RuntimeError("yandex endpoint is empty")
    api_key = (cfg.yandex_api_key or "").strip()
    iam_token = (cfg.yandex_iam_token or "").strip()
    if not api_key and not iam_token:
        raise RuntimeError("yandex credentials are missing (set api key or IAM token)")

    headers = {
        "Content-Type": "application/x-www-form-urlencoded",
    }
    if api_key:
        headers["Authorization"] = f"Api-Key {api_key}"
    else:
        headers["Authorization"] = f"Bearer {iam_token}"

    payload = {
        "text": text,
        "lang": (cfg.yandex_lang or "ru-RU").strip(),
        "voice": (cfg.yandex_voice or "ermil").strip(),
        "format": "lpcm",
        "sampleRateHertz": str(int(max(8000, min(48000, target_rate)))),
        "speed": f"{max(0.1, min(3.0, float(cfg.yandex_speed))):.3f}",
    }
    if cfg.yandex_folder_id:
        payload["folderId"] = cfg.yandex_folder_id.strip()
    if cfg.yandex_emotion:
        payload["emotion"] = cfg.yandex_emotion.strip()

    body = urllib.parse.urlencode(payload).encode("utf-8")
    req = urllib.request.Request(endpoint, data=body, headers=headers, method="POST")

    try:
        with urllib.request.urlopen(req, timeout=float(max(1.0, cfg.yandex_timeout_s))) as resp:
            raw = resp.read()
    except urllib.error.HTTPError as exc:
        err = exc.read().decode("utf-8", errors="ignore")
        raise RuntimeError(f"yandex http {exc.code}: {err}") from exc
    except Exception as exc:
        raise RuntimeError(f"yandex request failed: {exc}") from exc

    if not raw:
        raise RuntimeError("yandex returned empty audio")

    samples = np.frombuffer(raw, dtype="<i2")
    if samples.size == 0:
        raise RuntimeError("yandex returned non-lpcm payload")

    out_samples = _apply_pitch_rate_scale_i16(samples, cfg.piper_pitch_scale)
    out_samples = _apply_tempo_scale_i16(out_samples, cfg.tts_tempo_scale)
    out_samples = _apply_echo_i16(
        out_samples,
        target_rate,
        mix=cfg.tts_echo_mix,
        delay_ms=cfg.tts_echo_delay_ms,
        feedback=cfg.tts_echo_feedback,
    )
    out_samples = _apply_reverb_i16(
        out_samples,
        target_rate,
        mix=cfg.tts_reverb_mix,
        room_scale=cfg.tts_reverb_room_scale,
        damp=cfg.tts_reverb_damp,
    )
    out_samples = _postprocess_tail(
        out_samples,
        target_rate,
        fade_ms=cfg.tts_tail_fade_ms,
        silence_ms=cfg.tts_tail_silence_ms,
    )
    return TtsResult(
        pcm_le16=out_samples.astype("<i2", copy=False).tobytes(),
        sample_rate=target_rate,
        backend="yandex",
        model=(cfg.yandex_voice or "ermil").strip(),
    )


async def _synthesize_yandex(text: str, target_rate: int, cfg: TtsConfig) -> TtsResult:
    return await asyncio.to_thread(_yandex_synthesize_sync, text, target_rate, cfg)


def _synthesize_tone(text: str, target_rate: int, cfg: TtsConfig) -> TtsResult:
    dur_ms = max(120, int(cfg.tone_ms))
    hz = max(80.0, float(cfg.tone_hz))
    gain = _clamp_gain(cfg.tone_gain)
    n = max(1, int(target_rate * dur_ms / 1000))
    t = np.arange(n, dtype=np.float32) / float(target_rate)
    wave = np.sin((2.0 * math.pi * hz) * t)
    # Simple envelope to avoid clicks.
    ramp = max(1, int(0.01 * target_rate))
    env = np.ones_like(wave)
    env[:ramp] = np.linspace(0.0, 1.0, num=ramp, endpoint=False, dtype=np.float32)
    env[-ramp:] = np.linspace(1.0, 0.0, num=ramp, endpoint=False, dtype=np.float32)
    sig = wave * env * gain
    pcm = np.clip(sig * 32767.0, -32768.0, 32767.0).astype("<i2")
    return TtsResult(
        pcm_le16=pcm.tobytes(),
        sample_rate=target_rate,
        backend="tone",
        model=f"tone@{hz:.1f}Hz",
    )


async def synthesize_tts(text: str, sample_rate: int, voice_hint: str, cfg: TtsConfig) -> TtsResult:
    backend = cfg.backend.strip().lower()
    target_rate = max(8000, min(48000, int(sample_rate)))

    if backend == "piper":
        return await _synthesize_piper(text, target_rate, voice_hint, cfg)
    if backend == "silero":
        return await _synthesize_silero(text, target_rate, cfg)
    if backend == "yandex":
        return await _synthesize_yandex(text, target_rate, cfg)
    if backend == "tone":
        return _synthesize_tone(text, target_rate, cfg)
    raise RuntimeError("tts backend is disabled")


async def validate_tts_backend(cfg: TtsConfig) -> str:
    backend = cfg.backend.strip().lower()
    if backend == "none":
        return "disabled"
    if backend == "piper":
        model_path = _resolve_model_path(cfg, "")
        if model_path is None:
            model_dir = cfg.piper_model_dir.expanduser().resolve()
            raise RuntimeError(f"piper model not found in {model_dir}")
        piper_bin = cfg.piper_bin
        if any(sep in cfg.piper_bin for sep in ("/", "\\")) or cfg.piper_bin.startswith("~") or cfg.piper_bin.startswith("."):
            piper_path = Path(cfg.piper_bin).expanduser().resolve()
            if not piper_path.exists() or not piper_path.is_file():
                raise RuntimeError(f"piper binary not found: {piper_path}")
            piper_bin = str(piper_path)
        return f"piper:{piper_bin}:{model_path.name}"
    if backend == "silero":
        model, speaker_model = await asyncio.to_thread(_load_silero_model, cfg)
        await asyncio.to_thread(_silero_prewarm_sync, cfg)
        speaker = (cfg.silero_speaker or "").strip()
        if not speaker:
            speakers = getattr(model, "speakers", None)
            if isinstance(speakers, (list, tuple)) and len(speakers) > 0:
                speaker = str(speakers[0])
        if not speaker:
            speaker = "xenia"
        return f"silero:{speaker_model}:{speaker}"
    if backend == "yandex":
        api_key = (cfg.yandex_api_key or "").strip()
        iam_token = (cfg.yandex_iam_token or "").strip()
        if not api_key and not iam_token:
            raise RuntimeError("yandex credentials missing: set ORB_WS_YANDEX_API_KEY or ORB_WS_YANDEX_IAM_TOKEN")
        return f"yandex:{cfg.yandex_voice or 'ermil'}"
    if backend == "tone":
        return "tone"
    raise RuntimeError(f"unsupported tts backend: {backend}")
