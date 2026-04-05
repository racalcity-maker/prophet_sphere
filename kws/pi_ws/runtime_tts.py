from __future__ import annotations

import argparse
import json
import os
import time
import threading
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Dict

try:
    from .tts_synth import TtsConfig, synthesize_tts
except Exception:  # pragma: no cover
    from tts_synth import TtsConfig, synthesize_tts  # type: ignore


def now_ms() -> int:
    return int(time.time() * 1000)


_TTS_FX_PRESETS: Dict[str, Dict[str, float]] = {
    "off": {
        "echo_mix": 0.0,
        "echo_delay_ms": 170.0,
        "echo_feedback": 0.34,
        "reverb_mix": 0.0,
        "reverb_room_scale": 0.95,
        "reverb_damp": 0.28,
    },
    "soft": {
        "echo_mix": 0.10,
        "echo_delay_ms": 165.0,
        "echo_feedback": 0.28,
        "reverb_mix": 0.13,
        "reverb_room_scale": 0.90,
        "reverb_damp": 0.34,
    },
    "mystic": {
        "echo_mix": 0.24,
        "echo_delay_ms": 220.0,
        "echo_feedback": 0.46,
        "reverb_mix": 0.36,
        "reverb_room_scale": 1.35,
        "reverb_damp": 0.26,
    },
    "deep": {
        "echo_mix": 0.22,
        "echo_delay_ms": 230.0,
        "echo_feedback": 0.42,
        "reverb_mix": 0.30,
        "reverb_room_scale": 1.28,
        "reverb_damp": 0.26,
    },
}


def _env_float(name: str, default: float) -> float:
    raw = os.getenv(name, "").strip()
    if not raw:
        return default
    try:
        return float(raw)
    except Exception:
        return default


def _resolve_tts_fx(args: argparse.Namespace) -> Dict[str, float]:
    preset_name = str(getattr(args, "tts_fx_preset", "off")).strip().lower()
    if preset_name not in _TTS_FX_PRESETS:
        preset_name = "off"
    fx = dict(_TTS_FX_PRESETS[preset_name])

    # ENV overrides preset.
    fx["echo_mix"] = _env_float("ORB_WS_TTS_ECHO_MIX", fx["echo_mix"])
    fx["echo_delay_ms"] = _env_float("ORB_WS_TTS_ECHO_DELAY_MS", fx["echo_delay_ms"])
    fx["echo_feedback"] = _env_float("ORB_WS_TTS_ECHO_FEEDBACK", fx["echo_feedback"])
    fx["reverb_mix"] = _env_float("ORB_WS_TTS_REVERB_MIX", fx["reverb_mix"])
    fx["reverb_room_scale"] = _env_float("ORB_WS_TTS_REVERB_ROOM_SCALE", fx["reverb_room_scale"])
    fx["reverb_damp"] = _env_float("ORB_WS_TTS_REVERB_DAMP", fx["reverb_damp"])

    # Explicit CLI overrides both preset and ENV.
    for k in (
        "tts_echo_mix",
        "tts_echo_delay_ms",
        "tts_echo_feedback",
        "tts_reverb_mix",
        "tts_reverb_room_scale",
        "tts_reverb_damp",
    ):
        v = getattr(args, k, None)
        if v is None:
            continue
        if k == "tts_echo_mix":
            fx["echo_mix"] = float(v)
        elif k == "tts_echo_delay_ms":
            fx["echo_delay_ms"] = float(v)
        elif k == "tts_echo_feedback":
            fx["echo_feedback"] = float(v)
        elif k == "tts_reverb_mix":
            fx["reverb_mix"] = float(v)
        elif k == "tts_reverb_room_scale":
            fx["reverb_room_scale"] = float(v)
        elif k == "tts_reverb_damp":
            fx["reverb_damp"] = float(v)

    fx["preset"] = preset_name
    return fx

@dataclass(frozen=True)
class RuntimeTtsState:
    tts_cfg: TtsConfig
    tts_chunk_ms: int
    tts_segment_max_chars: int
    tts_phrase_pause_ms: int
    tts_pacing: str
    tts_pacing_lead_ms: int
    tts_chunk_min_bytes: int
    tts_fx_preset: str


class RuntimeTtsStore:
    def __init__(self, state: RuntimeTtsState, config_file: Path) -> None:
        self._state = state
        self._lock = threading.RLock()
        self._config_file = config_file

    def snapshot(self) -> RuntimeTtsState:
        with self._lock:
            return self._state

    def _norm_pacing(self, value: Any) -> str:
        pacing = str(value or "").strip().lower()
        if pacing not in ("realtime", "adaptive", "none"):
            raise ValueError("invalid tts_pacing")
        return pacing

    def _norm_backend(self, value: Any) -> str:
        backend = str(value or "").strip().lower()
        if backend not in ("none", "piper", "silero", "yandex", "tone"):
            raise ValueError("invalid tts_backend")
        return backend

    def _norm_bool(self, value: Any) -> bool:
        if isinstance(value, bool):
            return value
        if isinstance(value, (int, float)):
            return bool(value)
        text = str(value).strip().lower()
        if text in ("1", "true", "yes", "on"):
            return True
        if text in ("0", "false", "no", "off"):
            return False
        raise ValueError("invalid boolean")

    def _norm_int(self, value: Any, *, lo: int, hi: int, name: str) -> int:
        try:
            iv = int(value)
        except Exception as exc:
            raise ValueError(f"invalid {name}") from exc
        if iv < lo or iv > hi:
            raise ValueError(f"invalid {name}")
        return iv

    def _norm_float(self, value: Any, *, lo: float, hi: float, name: str) -> float:
        try:
            fv = float(value)
        except Exception as exc:
            raise ValueError(f"invalid {name}") from exc
        if fv < lo or fv > hi:
            raise ValueError(f"invalid {name}")
        return fv

    def _apply_preset(self, cfg: TtsConfig, preset: str) -> TtsConfig:
        base = _TTS_FX_PRESETS.get(preset, _TTS_FX_PRESETS["off"])
        return replace(
            cfg,
            tts_echo_mix=float(base["echo_mix"]),
            tts_echo_delay_ms=int(base["echo_delay_ms"]),
            tts_echo_feedback=float(base["echo_feedback"]),
            tts_reverb_mix=float(base["reverb_mix"]),
            tts_reverb_room_scale=float(base["reverb_room_scale"]),
            tts_reverb_damp=float(base["reverb_damp"]),
        )

    def update_from_patch(self, patch: Dict[str, Any]) -> RuntimeTtsState:
        if not isinstance(patch, dict):
            raise ValueError("patch must be object")
        with self._lock:
            state = self._state
            cfg = state.tts_cfg
            prev_fx_cfg = state.tts_cfg
            preset = state.tts_fx_preset
            preset_applied = False
            chunk_ms = state.tts_chunk_ms
            segment_max_chars = state.tts_segment_max_chars
            phrase_pause_ms = state.tts_phrase_pause_ms
            pacing = state.tts_pacing
            pacing_lead_ms = state.tts_pacing_lead_ms
            chunk_min_bytes = state.tts_chunk_min_bytes

            if "tts_fx_preset" in patch:
                candidate = str(patch["tts_fx_preset"]).strip().lower()
                if candidate not in _TTS_FX_PRESETS:
                    raise ValueError("invalid tts_fx_preset")
                preset = candidate
                cfg = self._apply_preset(cfg, preset)
                preset_applied = True

            if "tts_backend" in patch:
                cfg = replace(cfg, backend=self._norm_backend(patch["tts_backend"]))
            if "tts_pacing" in patch:
                pacing = self._norm_pacing(patch["tts_pacing"])
            if "tts_chunk_ms" in patch:
                chunk_ms = self._norm_int(patch["tts_chunk_ms"], lo=5, hi=200, name="tts_chunk_ms")
            if "tts_chunk_min_bytes" in patch:
                chunk_min_bytes = self._norm_int(patch["tts_chunk_min_bytes"], lo=128, hi=65536, name="tts_chunk_min_bytes")
            if "tts_segment_max_chars" in patch:
                segment_max_chars = self._norm_int(patch["tts_segment_max_chars"], lo=32, hi=1200, name="tts_segment_max_chars")
            if "tts_phrase_pause_ms" in patch:
                phrase_pause_ms = self._norm_int(patch["tts_phrase_pause_ms"], lo=0, hi=10000, name="tts_phrase_pause_ms")
            if "tts_pacing_lead_ms" in patch:
                pacing_lead_ms = self._norm_int(patch["tts_pacing_lead_ms"], lo=0, hi=2000, name="tts_pacing_lead_ms")

            # Piper / shared controls
            if "piper_default_model" in patch:
                cfg = replace(cfg, piper_default_model=str(patch["piper_default_model"]).strip())
            if "piper_length_scale" in patch:
                cfg = replace(cfg, piper_length_scale=self._norm_float(patch["piper_length_scale"], lo=0.5, hi=2.5, name="piper_length_scale"))
            if "piper_noise_scale" in patch:
                cfg = replace(cfg, piper_noise_scale=self._norm_float(patch["piper_noise_scale"], lo=0.0, hi=2.0, name="piper_noise_scale"))
            if "piper_noise_w" in patch:
                cfg = replace(cfg, piper_noise_w=self._norm_float(patch["piper_noise_w"], lo=0.0, hi=2.0, name="piper_noise_w"))
            if "piper_sentence_silence_s" in patch:
                cfg = replace(cfg, piper_sentence_silence_s=self._norm_float(patch["piper_sentence_silence_s"], lo=0.0, hi=2.0, name="piper_sentence_silence_s"))
            if "piper_pitch_scale" in patch:
                cfg = replace(cfg, piper_pitch_scale=self._norm_float(patch["piper_pitch_scale"], lo=0.7, hi=1.5, name="piper_pitch_scale"))
            if "tts_tempo_scale" in patch:
                cfg = replace(cfg, tts_tempo_scale=self._norm_float(patch["tts_tempo_scale"], lo=0.50, hi=1.4, name="tts_tempo_scale"))

            # Silero controls
            if "silero_language" in patch:
                cfg = replace(cfg, silero_language=str(patch["silero_language"]).strip())
            if "silero_speaker_model" in patch:
                cfg = replace(cfg, silero_speaker_model=str(patch["silero_speaker_model"]).strip())
            if "silero_speaker" in patch:
                cfg = replace(cfg, silero_speaker=str(patch["silero_speaker"]).strip())
            if "silero_sample_rate" in patch:
                cfg = replace(cfg, silero_sample_rate=self._norm_int(patch["silero_sample_rate"], lo=8000, hi=48000, name="silero_sample_rate"))
            if "silero_put_accent" in patch:
                cfg = replace(cfg, silero_put_accent=self._norm_bool(patch["silero_put_accent"]))
            if "silero_put_yo" in patch:
                cfg = replace(cfg, silero_put_yo=self._norm_bool(patch["silero_put_yo"]))
            if "silero_num_threads" in patch:
                cfg = replace(cfg, silero_num_threads=self._norm_int(patch["silero_num_threads"], lo=1, hi=8, name="silero_num_threads"))

            # Yandex controls
            if "yandex_endpoint" in patch:
                cfg = replace(cfg, yandex_endpoint=str(patch["yandex_endpoint"]).strip())
            if "yandex_api_key" in patch:
                cfg = replace(cfg, yandex_api_key=str(patch["yandex_api_key"]).strip())
            if "yandex_iam_token" in patch:
                cfg = replace(cfg, yandex_iam_token=str(patch["yandex_iam_token"]).strip())
            if "yandex_folder_id" in patch:
                cfg = replace(cfg, yandex_folder_id=str(patch["yandex_folder_id"]).strip())
            if "yandex_lang" in patch:
                cfg = replace(cfg, yandex_lang=str(patch["yandex_lang"]).strip())
            if "yandex_voice" in patch:
                cfg = replace(cfg, yandex_voice=str(patch["yandex_voice"]).strip())
            if "yandex_speed" in patch:
                cfg = replace(cfg, yandex_speed=self._norm_float(patch["yandex_speed"], lo=0.1, hi=3.0, name="yandex_speed"))
            if "yandex_emotion" in patch:
                cfg = replace(cfg, yandex_emotion=str(patch["yandex_emotion"]).strip())
            if "yandex_timeout_s" in patch:
                cfg = replace(cfg, yandex_timeout_s=self._norm_float(patch["yandex_timeout_s"], lo=1.0, hi=120.0, name="yandex_timeout_s"))

            # FX controls
            suppress_stale_fx_override = False
            fx_keys = (
                "tts_echo_mix",
                "tts_echo_delay_ms",
                "tts_echo_feedback",
                "tts_reverb_mix",
                "tts_reverb_room_scale",
                "tts_reverb_damp",
            )
            if preset_applied and all(k in patch for k in fx_keys):
                try:
                    in_echo_mix = self._norm_float(patch["tts_echo_mix"], lo=0.0, hi=1.0, name="tts_echo_mix")
                    in_echo_delay = self._norm_int(patch["tts_echo_delay_ms"], lo=20, hi=1000, name="tts_echo_delay_ms")
                    in_echo_fb = self._norm_float(patch["tts_echo_feedback"], lo=0.0, hi=0.95, name="tts_echo_feedback")
                    in_rev_mix = self._norm_float(patch["tts_reverb_mix"], lo=0.0, hi=1.0, name="tts_reverb_mix")
                    in_rev_room = self._norm_float(patch["tts_reverb_room_scale"], lo=0.2, hi=2.0, name="tts_reverb_room_scale")
                    in_rev_damp = self._norm_float(patch["tts_reverb_damp"], lo=0.0, hi=1.0, name="tts_reverb_damp")

                    # Web UI sends all FX numeric fields on each save.
                    # If they still match the previous runtime values exactly,
                    # treat them as stale mirror values and keep the new preset.
                    def _eqf(a: float, b: float) -> bool:
                        return abs(float(a) - float(b)) <= 1e-6

                    suppress_stale_fx_override = (
                        _eqf(in_echo_mix, prev_fx_cfg.tts_echo_mix)
                        and int(in_echo_delay) == int(prev_fx_cfg.tts_echo_delay_ms)
                        and _eqf(in_echo_fb, prev_fx_cfg.tts_echo_feedback)
                        and _eqf(in_rev_mix, prev_fx_cfg.tts_reverb_mix)
                        and _eqf(in_rev_room, prev_fx_cfg.tts_reverb_room_scale)
                        and _eqf(in_rev_damp, prev_fx_cfg.tts_reverb_damp)
                    )
                except Exception:
                    suppress_stale_fx_override = False

            if not suppress_stale_fx_override:
                if "tts_echo_mix" in patch:
                    cfg = replace(cfg, tts_echo_mix=self._norm_float(patch["tts_echo_mix"], lo=0.0, hi=1.0, name="tts_echo_mix"))
                if "tts_echo_delay_ms" in patch:
                    cfg = replace(cfg, tts_echo_delay_ms=self._norm_int(patch["tts_echo_delay_ms"], lo=20, hi=1000, name="tts_echo_delay_ms"))
                if "tts_echo_feedback" in patch:
                    cfg = replace(cfg, tts_echo_feedback=self._norm_float(patch["tts_echo_feedback"], lo=0.0, hi=0.95, name="tts_echo_feedback"))
                if "tts_reverb_mix" in patch:
                    cfg = replace(cfg, tts_reverb_mix=self._norm_float(patch["tts_reverb_mix"], lo=0.0, hi=1.0, name="tts_reverb_mix"))
                if "tts_reverb_room_scale" in patch:
                    cfg = replace(cfg, tts_reverb_room_scale=self._norm_float(patch["tts_reverb_room_scale"], lo=0.2, hi=2.0, name="tts_reverb_room_scale"))
                if "tts_reverb_damp" in patch:
                    cfg = replace(cfg, tts_reverb_damp=self._norm_float(patch["tts_reverb_damp"], lo=0.0, hi=1.0, name="tts_reverb_damp"))
            if "tts_tail_fade_ms" in patch:
                cfg = replace(cfg, tts_tail_fade_ms=self._norm_int(patch["tts_tail_fade_ms"], lo=0, hi=2000, name="tts_tail_fade_ms"))
            if "tts_tail_silence_ms" in patch:
                cfg = replace(cfg, tts_tail_silence_ms=self._norm_int(patch["tts_tail_silence_ms"], lo=0, hi=4000, name="tts_tail_silence_ms"))

            new_state = RuntimeTtsState(
                tts_cfg=cfg,
                tts_chunk_ms=chunk_ms,
                tts_segment_max_chars=segment_max_chars,
                tts_phrase_pause_ms=phrase_pause_ms,
                tts_pacing=pacing,
                tts_pacing_lead_ms=pacing_lead_ms,
                tts_chunk_min_bytes=chunk_min_bytes,
                tts_fx_preset=preset,
            )
            self._state = new_state
            return new_state

    def to_public_dict(self) -> Dict[str, Any]:
        state = self.snapshot()
        cfg = state.tts_cfg
        return {
            "tts_backend": cfg.backend,
            "tts_pacing": state.tts_pacing,
            "tts_chunk_ms": state.tts_chunk_ms,
            "tts_chunk_min_bytes": state.tts_chunk_min_bytes,
            "tts_segment_max_chars": state.tts_segment_max_chars,
            "tts_phrase_pause_ms": state.tts_phrase_pause_ms,
            "tts_pacing_lead_ms": state.tts_pacing_lead_ms,
            "tts_fx_preset": state.tts_fx_preset,
            "piper_default_model": cfg.piper_default_model,
            "piper_length_scale": cfg.piper_length_scale,
            "piper_noise_scale": cfg.piper_noise_scale,
            "piper_noise_w": cfg.piper_noise_w,
            "piper_sentence_silence_s": cfg.piper_sentence_silence_s,
            "piper_pitch_scale": cfg.piper_pitch_scale,
            "tts_tempo_scale": cfg.tts_tempo_scale,
            "silero_language": cfg.silero_language,
            "silero_speaker_model": cfg.silero_speaker_model,
            "silero_speaker": cfg.silero_speaker,
            "silero_sample_rate": cfg.silero_sample_rate,
            "silero_put_accent": cfg.silero_put_accent,
            "silero_put_yo": cfg.silero_put_yo,
            "silero_num_threads": cfg.silero_num_threads,
            "yandex_endpoint": cfg.yandex_endpoint,
            "yandex_api_key": cfg.yandex_api_key,
            "yandex_iam_token": cfg.yandex_iam_token,
            "yandex_folder_id": cfg.yandex_folder_id,
            "yandex_lang": cfg.yandex_lang,
            "yandex_voice": cfg.yandex_voice,
            "yandex_speed": cfg.yandex_speed,
            "yandex_emotion": cfg.yandex_emotion,
            "yandex_timeout_s": cfg.yandex_timeout_s,
            "tts_echo_mix": cfg.tts_echo_mix,
            "tts_echo_delay_ms": cfg.tts_echo_delay_ms,
            "tts_echo_feedback": cfg.tts_echo_feedback,
            "tts_reverb_mix": cfg.tts_reverb_mix,
            "tts_reverb_room_scale": cfg.tts_reverb_room_scale,
            "tts_reverb_damp": cfg.tts_reverb_damp,
            "tts_tail_fade_ms": cfg.tts_tail_fade_ms,
            "tts_tail_silence_ms": cfg.tts_tail_silence_ms,
            "config_file": str(self._config_file),
        }

    def save(self) -> None:
        payload = self.to_public_dict()
        payload["_schema"] = "orb_tts_runtime_v1"
        payload["_saved_ms"] = now_ms()
        self._config_file.parent.mkdir(parents=True, exist_ok=True)
        self._config_file.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")

    def load(self) -> RuntimeTtsState:
        if not self._config_file.exists():
            raise FileNotFoundError(str(self._config_file))
        raw = json.loads(self._config_file.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            raise ValueError("invalid config file")
        patch = dict(raw)
        patch.pop("_schema", None)
        patch.pop("_saved_ms", None)
        patch.pop("config_file", None)
        return self.update_from_patch(patch)


async def warmup_tts_runtime(
    *,
    tts_cfg: TtsConfig,
    passes: int,
    sample_rate: int,
    text: str,
) -> None:
    backend = (tts_cfg.backend or "").strip().lower()
    if backend in ("", "none"):
        return
    phrase = (text or "").strip()
    if not phrase:
        phrase = "Проверка голоса. Система готова."
    total = max(1, int(passes))
    rate = max(8000, min(48000, int(sample_rate)))
    t0 = time.perf_counter()
    for idx in range(total):
        _ = await synthesize_tts(phrase, rate, "", tts_cfg)
        dt = int((time.perf_counter() - t0) * 1000.0)
        print(f"[warmup] tts pass={idx + 1}/{total} backend={backend} sr={rate} elapsed_ms={dt}")


def _build_runtime_tts_state(args: argparse.Namespace) -> RuntimeTtsState:
    fx = _resolve_tts_fx(args)
    tts_cfg = TtsConfig(
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
        tts_echo_mix=float(fx["echo_mix"]),
        tts_echo_delay_ms=int(fx["echo_delay_ms"]),
        tts_echo_feedback=float(fx["echo_feedback"]),
        tts_reverb_mix=float(fx["reverb_mix"]),
        tts_reverb_room_scale=float(fx["reverb_room_scale"]),
        tts_reverb_damp=float(fx["reverb_damp"]),
        tts_tail_fade_ms=int(args.tts_tail_fade_ms),
        tts_tail_silence_ms=int(args.tts_tail_silence_ms),
        tone_hz=float(args.tts_tone_hz),
        tone_ms=int(args.tts_tone_ms),
        tone_gain=float(args.tts_tone_gain),
    )
    return RuntimeTtsState(
        tts_cfg=tts_cfg,
        tts_chunk_ms=int(args.tts_chunk_ms),
        tts_segment_max_chars=int(args.tts_segment_max_chars),
        tts_phrase_pause_ms=int(args.tts_phrase_pause_ms),
        tts_pacing=str(args.tts_pacing).strip().lower(),
        tts_pacing_lead_ms=int(args.tts_pacing_lead_ms),
        tts_chunk_min_bytes=int(args.tts_chunk_min_bytes),
        tts_fx_preset=str(fx["preset"]),
    )
