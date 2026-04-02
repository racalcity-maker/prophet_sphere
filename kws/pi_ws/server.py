#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import numpy as np

try:
    # Preferred package imports (python -m kws.pi_ws.server)
    from .asr_vosk import make_vosk_session
    from .intent_from_text import VALID_INTENTS, infer_intent_from_text, load_intent_keywords
    from .oracle_llm import OracleLlmConfig, OracleLlmInput, build_oracle_llm
    from .reasoner import ReasonerConfig, ReasonerInput, build_reasoner
    from .text_bank import OracleTextBank
    from .tts_synth import TtsConfig, synthesize_tts, validate_tts_backend
except Exception:  # pragma: no cover
    # Fallback when running as a plain script file
    from asr_vosk import make_vosk_session  # type: ignore
    from intent_from_text import VALID_INTENTS, infer_intent_from_text, load_intent_keywords  # type: ignore
    from oracle_llm import OracleLlmConfig, OracleLlmInput, build_oracle_llm  # type: ignore
    from reasoner import ReasonerConfig, ReasonerInput, build_reasoner  # type: ignore
    from text_bank import OracleTextBank  # type: ignore
    from tts_synth import TtsConfig, synthesize_tts, validate_tts_backend  # type: ignore

try:
    from websockets.asyncio.server import serve
except Exception:  # pragma: no cover
    from websockets.server import serve  # type: ignore

try:
    from websockets.exceptions import ConnectionClosed
except Exception:  # pragma: no cover
    ConnectionClosed = Exception  # type: ignore


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


def now_ms() -> int:
    return int(time.time() * 1000)


def parse_json_text(text: str) -> Optional[dict]:
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        return None
    return value if isinstance(value, dict) else None


def pcm_diag(pcm_le16: bytes) -> Dict[str, float]:
    if not pcm_le16:
        return {"samples": 0.0, "rms": 0.0, "peak": 0.0, "dc": 0.0, "clip_ratio": 0.0}
    s = np.frombuffer(pcm_le16, dtype="<i2").astype(np.int32)
    if s.size == 0:
        return {"samples": 0.0, "rms": 0.0, "peak": 0.0, "dc": 0.0, "clip_ratio": 0.0}
    peak = int(np.max(np.abs(s)))
    rms = float(np.sqrt(np.mean((s.astype(np.float64)) ** 2)))
    dc = float(np.mean(s))
    clip = float(np.mean(np.abs(s) >= 32760))
    return {
        "samples": float(s.size),
        "rms": rms,
        "peak": float(peak),
        "dc": dc,
        "clip_ratio": clip,
    }


def default_intent_map_path() -> Path:
    return Path(__file__).resolve().with_name("intent_keywords_ru.json")


def default_oracle_texts_dir() -> Path:
    # .../kws/pi_ws/server.py -> .../ (repo root on Pi mirror)
    return Path(__file__).resolve().parents[2] / "docs" / "texts"


def default_stress_map_path() -> Path:
    return Path(__file__).resolve().with_name("stress_overrides_ru.json")


@dataclass
class StressOverrides:
    words: Dict[str, str] = field(default_factory=dict)
    phrases: List[Tuple[str, str]] = field(default_factory=list)


_STRESS_WORD_RE = re.compile(r"[A-Za-zА-Яа-яЁё-]+", flags=re.IGNORECASE)


def _norm_stress_key(value: str) -> str:
    return value.strip().lower().replace("ё", "е")


def load_stress_overrides(path: Path) -> StressOverrides:
    raw = json.loads(path.read_text(encoding="utf-8"))
    words: Dict[str, str] = {}
    phrases: List[Tuple[str, str]] = []

    if isinstance(raw, dict):
        words_src = raw.get("words", raw)
        phrases_src = raw.get("phrases", {})
        if isinstance(words_src, dict):
            for k, v in words_src.items():
                if not isinstance(k, str) or not isinstance(v, str):
                    continue
                nk = _norm_stress_key(k)
                vv = v.strip()
                if nk and vv:
                    words[nk] = vv
        if isinstance(phrases_src, dict):
            for k, v in phrases_src.items():
                if not isinstance(k, str) or not isinstance(v, str):
                    continue
                kk = k.strip()
                vv = v.strip()
                if kk and vv:
                    phrases.append((kk, vv))

    # Longest phrases first to avoid partial override collisions.
    phrases.sort(key=lambda x: len(x[0]), reverse=True)
    return StressOverrides(words=words, phrases=phrases)


def apply_stress_overrides(text: str, overrides: Optional[StressOverrides]) -> str:
    if not text or overrides is None:
        return text
    out = text
    for src, dst in overrides.phrases:
        out = re.sub(re.escape(src), dst, out, flags=re.IGNORECASE)

    if not overrides.words:
        return out

    def _replace_word(match: re.Match[str]) -> str:
        token = match.group(0)
        if "+" in token:
            return token
        repl = overrides.words.get(_norm_stress_key(token))
        return repl if repl else token

    return _STRESS_WORD_RE.sub(_replace_word, out)


def strip_tts_stress_markers(text: str) -> str:
    if not text:
        return text
    # Keep stress map compatibility but never pass '+' markers to TTS backend.
    # Preserve line boundaries: one line == one TTS segment.
    lines = text.replace("+", "").splitlines()
    normalized: list[str] = []
    for line in lines:
        compact = re.sub(r"[ \t]+", " ", line).strip()
        if compact:
            normalized.append(compact)
    return "\n".join(normalized).strip()


def normalize_tts_punctuation(text: str) -> str:
    clean = text.strip()
    if not clean:
        return ""

    # Keep author punctuation as-is; only normalize whitespace noise.
    clean = re.sub(r"\s+", " ", clean)
    return clean.strip()


def split_tts_text(text: str, max_chars: int) -> list[str]:
    raw = (text or "").strip()
    if not raw:
        return []

    # Strict phrase segmentation: one input line == one TTS segment.
    # We do not split by punctuation to preserve authored intonation.
    # We do not split/merge by max_chars to avoid mid-phrase artifacts.
    _ = max_chars  # kept for API compatibility
    chunks: list[str] = []
    line_parts = [normalize_tts_punctuation(ln) for ln in re.split(r"[\r\n]+", raw)]
    for line in line_parts:
        if line:
            chunks.append(line)

    if not chunks:
        fallback = normalize_tts_punctuation(raw)
        if fallback:
            chunks.append(fallback)

    return chunks


async def resolve_tts_text(
    raw_text: str,
    *,
    oracle_bank: Optional[OracleTextBank],
    oracle_llm: Any,
    last_intent: str,
    last_question_form: str,
    last_prediction_polarity: str,
    last_transcript: str,
    last_confidence: float,
) -> tuple[str, str]:
    key = (raw_text or "").strip()
    if key == "__ORACLE_INTRO__":
        if oracle_bank is not None:
            intro = oracle_bank.pick_service_phrase(
                "intro",
                fallback="Приветствую. Я слушаю тебя внимательно.",
            )
            if intro:
                return intro, "oracle_bank_intro"
        return "Приветствую. Я слушаю тебя внимательно.", "oracle_intro_fallback"
    if key == "__ORACLE_AUTO__":
        if oracle_llm is not None:
            try:
                scripted = await oracle_llm.generate(
                    OracleLlmInput(
                        text=last_transcript,
                        intent=last_intent,
                        question_form=last_question_form,
                        polarity=last_prediction_polarity,
                        confidence=float(last_confidence),
                    )
                )
                if scripted is not None and scripted.text:
                    return scripted.text, f"oracle_llm provider={scripted.provider}"
            except Exception:
                pass
        if oracle_bank is not None:
            selected = oracle_bank.select_script(
                intent=last_intent,
                question_form=last_question_form,
                force_polarity=last_prediction_polarity,
            )
            if selected.text:
                source = (
                    f"oracle_bank domain={selected.domain} form={selected.question_form} "
                    f"prediction={selected.prediction_bucket}"
                )
                return selected.text, source
        return "", "oracle_bank_empty"
    return raw_text, "plain_text"


@dataclass
class CaptureState:
    capture_id: int = 0
    sample_rate: int = 16000
    started_ms: int = 0
    pcm: bytearray = field(default_factory=bytearray)

    def active(self) -> bool:
        return self.capture_id != 0

    def reset(self) -> None:
        self.capture_id = 0
        self.sample_rate = 16000
        self.started_ms = 0
        self.pcm.clear()


@dataclass
class PendingCapture:
    capture_id: int
    sample_rate: int
    pcm: bytes
    stored_ms: int


_PENDING_CAPTURE_MAX_AGE_MS = 15000
_pending_captures: Dict[int, PendingCapture] = {}


def _prune_pending_captures(now: int) -> None:
    stale_ids = [cid for cid, item in _pending_captures.items() if (now - item.stored_ms) > _PENDING_CAPTURE_MAX_AGE_MS]
    for cid in stale_ids:
        _pending_captures.pop(cid, None)


async def send_result(ws: Any, capture_id: int, intent: str, conf: float) -> None:
    payload = {
        "type": "result",
        "capture_id": int(capture_id),
        "intent": intent,
        "confidence": float(conf),
    }
    await ws.send(json.dumps(payload, ensure_ascii=False))


async def send_tts_control(ws: Any, *, ok: bool, error: str = "") -> None:
    payload: Dict[str, Any] = {"type": "tts_done" if ok else "tts_error"}
    if not ok and error:
        payload["error"] = error
    await ws.send(json.dumps(payload, ensure_ascii=False))


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


async def handle_client(
    ws: Any,
    *,
    vosk_model: str,
    intent_keywords: Any,
    reasoner: Any,
    tts_cfg: TtsConfig,
    tts_chunk_ms: int,
    tts_segment_max_chars: int,
    tts_phrase_pause_ms: int,
    tts_pacing: str,
    tts_pacing_lead_ms: int,
    tts_chunk_min_bytes: int,
    oracle_bank: Optional[OracleTextBank],
    oracle_llm: Any,
    stress_overrides: Optional[StressOverrides],
) -> None:
    conn_id = f"{id(ws):x}"
    state = CaptureState()
    vosk = make_vosk_session(vosk_model, sample_rate=16000)
    path = getattr(ws, "path", "/mic")
    print(f"[conn {conn_id}] open path={path}")
    if vosk is None:
        print(f"[conn {conn_id}] ERROR backend=vosk unavailable (model/package missing)")
        await ws.close(code=1011, reason="vosk unavailable")
        return
    print(f"[conn {conn_id}] backend=vosk model={vosk_model}")

    last_intent = "unknown"
    last_question_form = "open"
    last_prediction_polarity = "neutral"
    last_transcript = ""
    last_confidence = 0.0

    try:
        async for message in ws:
            if isinstance(message, bytes):
                if state.active():
                    state.pcm.extend(message)
                    vosk.feed_pcm16(message)
                continue

            obj = parse_json_text(message)
            if obj is None:
                print(f"[conn {conn_id}] bad json: {message[:120]}")
                continue

            msg_type = str(obj.get("type", "")).strip().lower()
            if msg_type == "tts":
                requested_text = str(obj.get("text", "")).strip()
                text, text_source = await resolve_tts_text(
                    requested_text,
                    oracle_bank=oracle_bank,
                    oracle_llm=oracle_llm,
                    last_intent=last_intent,
                    last_question_form=last_question_form,
                    last_prediction_polarity=last_prediction_polarity,
                    last_transcript=last_transcript,
                    last_confidence=last_confidence,
                )
                text = apply_stress_overrides(text, stress_overrides)
                text = strip_tts_stress_markers(text)
                if not text:
                    await send_tts_control(ws, ok=False, error="empty_text")
                    print(f"[conn {conn_id}] tts error: empty_text")
                    continue
                if state.active():
                    await send_tts_control(ws, ok=False, error="capture_active")
                    print(f"[conn {conn_id}] tts error: capture_active id={state.capture_id}")
                    continue

                req_rate = int(obj.get("sample_rate", 16000))
                req_rate = max(8000, min(48000, req_rate))
                voice = ""

                try:
                    print(
                        f"[conn {conn_id}] [tts] request source={text_source} "
                        f"intent={last_intent} form={last_question_form} polarity={last_prediction_polarity} "
                        f"chars={len(text)}"
                    )
                    t0 = now_ms()
                    req_t0 = time.perf_counter()
                    segments = split_tts_text(text, tts_segment_max_chars)
                    seg_count = len(segments)
                    if seg_count == 0:
                        raise RuntimeError("empty_tts_segments")
                    effective_phrase_pause_ms = int(tts_phrase_pause_ms)
                    if (
                        effective_phrase_pause_ms <= 0
                        and text_source.startswith("oracle_bank")
                        and seg_count > 1
                    ):
                        effective_phrase_pause_ms = 1500
                        print(
                            f"[conn {conn_id}] [tts] phrase pause override for oracle script: "
                            f"{effective_phrase_pause_ms}ms"
                        )
                    total = 0
                    total_audio_ms = 0
                    first_chunk_sent = False
                    synth_task = asyncio.create_task(synthesize_tts(segments[0], req_rate, voice, tts_cfg))
                    for seg_idx in range(seg_count):
                        seg_text = segments[seg_idx]
                        synth_t0 = time.perf_counter()
                        tts = await synth_task
                        synth_ms = int((time.perf_counter() - synth_t0) * 1000.0)
                        if seg_idx + 1 < seg_count:
                            synth_task = asyncio.create_task(
                                synthesize_tts(segments[seg_idx + 1], req_rate, voice, tts_cfg)
                            )
                        chunk_samples = max(80, int(tts.sample_rate * max(1, tts_chunk_ms) / 1000))
                        chunk_bytes = max(160, chunk_samples * 2)
                        chunk_bytes = max(chunk_bytes, max(256, int(tts_chunk_min_bytes)))
                        # Keep 16-bit sample alignment.
                        if (chunk_bytes & 1) != 0:
                            chunk_bytes += 1
                        seg_bytes = len(tts.pcm_le16)
                        seg_audio_ms = int((seg_bytes / 2) * 1000 / max(1, tts.sample_rate))
                        sent = 0
                        sent_samples = 0
                        seg_t0 = time.perf_counter()
                        total += seg_bytes
                        total_audio_ms += seg_audio_ms
                        print(
                            f"[conn {conn_id}] [tts] segment {seg_idx + 1}/{seg_count} "
                            f"backend={tts.backend} model={tts.model} sr={tts.sample_rate} "
                            f"bytes={seg_bytes} chunk={chunk_bytes} synth_ms={synth_ms} "
                            f"chars={len(seg_text)} audio_ms={seg_audio_ms}"
                        )
                        if seg_idx == 0:
                            # One-time pre-roll silence to suppress pop on stream start.
                            pre_roll_ms = 24
                            pre_roll_samples = max(1, int(tts.sample_rate * pre_roll_ms / 1000.0))
                            pre_roll_bytes_total = pre_roll_samples * 2
                            pre_roll_sent = 0
                            pre_roll_t0 = time.perf_counter()
                            while pre_roll_sent < pre_roll_bytes_total:
                                nxt = min(pre_roll_bytes_total, pre_roll_sent + chunk_bytes)
                                await ws.send(b"\x00" * (nxt - pre_roll_sent))
                                pre_roll_sent = nxt
                                if tts_pacing in ("realtime", "adaptive"):
                                    pre_progress_s = (pre_roll_sent / 2) / float(max(1, tts.sample_rate))
                                    target_t = pre_roll_t0 + pre_progress_s
                                    delay_s = target_t - time.perf_counter()
                                    if delay_s > 0.0:
                                        await asyncio.sleep(delay_s if tts_pacing == "realtime" else min(delay_s, 0.03))
                            print(
                                f"[conn {conn_id}] [tts] pre_roll_silence_ms={pre_roll_ms} "
                                f"audio_ms={pre_roll_ms}"
                            )
                        while sent < seg_bytes:
                            nxt = min(seg_bytes, sent + chunk_bytes)
                            await ws.send(tts.pcm_le16[sent:nxt])
                            if not first_chunk_sent:
                                first_chunk_sent = True
                                first_ms = int((time.perf_counter() - req_t0) * 1000.0)
                                print(f"[conn {conn_id}] [tts] first_chunk_sent_ms={first_ms}")
                            sent_samples += (nxt - sent) // 2
                            sent = nxt
                            if tts_pacing == "realtime":
                                # Pace by absolute audio timeline to avoid per-chunk drift.
                                lead_s = float(max(0, tts_pacing_lead_ms)) / 1000.0
                                target_t = seg_t0 + (float(sent_samples) / float(max(1, tts.sample_rate))) - lead_s
                                delay_s = target_t - time.perf_counter()
                                if delay_s > 0.0:
                                    await asyncio.sleep(delay_s)
                            elif tts_pacing == "adaptive":
                                # Adaptive pacing: only sleep when stream is ahead of real-time.
                                lead_s = float(max(0, tts_pacing_lead_ms)) / 1000.0
                                target_t = seg_t0 + (float(sent_samples) / float(max(1, tts.sample_rate))) - lead_s
                                delay_s = target_t - time.perf_counter()
                                if delay_s > 0.0:
                                    await asyncio.sleep(min(delay_s, 0.03))
                        seg_send_ms = int((time.perf_counter() - seg_t0) * 1000.0)
                        seg_rtf = (float(seg_send_ms) / float(max(1, seg_audio_ms))) if seg_audio_ms > 0 else 0.0
                        print(
                            f"[conn {conn_id}] [tts] segment {seg_idx + 1}/{seg_count} "
                            f"send_ms={seg_send_ms} audio_ms={seg_audio_ms} send_rtf={seg_rtf:.3f}"
                        )
                        if seg_idx + 1 < seg_count and effective_phrase_pause_ms > 0:
                            pause_samples = max(1, int(tts.sample_rate * effective_phrase_pause_ms / 1000.0))
                            pause_bytes_total = pause_samples * 2
                            pause_sent = 0
                            pause_t0 = time.perf_counter()
                            while pause_sent < pause_bytes_total:
                                nxt = min(pause_bytes_total, pause_sent + chunk_bytes)
                                await ws.send(b"\x00" * (nxt - pause_sent))
                                pause_sent = nxt
                                if tts_pacing in ("realtime", "adaptive"):
                                    pause_progress_s = (pause_sent / 2) / float(max(1, tts.sample_rate))
                                    target_t = pause_t0 + pause_progress_s
                                    delay_s = target_t - time.perf_counter()
                                    if delay_s > 0.0:
                                        await asyncio.sleep(delay_s if tts_pacing == "realtime" else min(delay_s, 0.03))
                            print(
                                f"[conn {conn_id}] [tts] inter_phrase_pause_ms={effective_phrase_pause_ms} "
                                f"audio_ms={effective_phrase_pause_ms}"
                            )
                    await send_tts_control(ws, ok=True)
                    dt = now_ms() - t0
                    rtf = (float(dt) / float(max(1, total_audio_ms))) if total_audio_ms > 0 else 0.0
                    print(
                        f"[conn {conn_id}] [tts] done segments={seg_count} bytes={total} "
                        f"send_ms={dt} audio_ms={total_audio_ms} pacing={tts_pacing} rtf={rtf:.3f}"
                    )
                except Exception as exc:
                    err = str(exc).strip() or "tts_failed"
                    await send_tts_control(ws, ok=False, error=err[:120])
                    print(f"[conn {conn_id}] [tts] error: {err}")
                continue

            if msg_type == "start":
                capture_id = int(obj.get("capture_id", 0))
                sample_rate = int(obj.get("sample_rate", 16000))
                if capture_id <= 0:
                    print(f"[conn {conn_id}] invalid start capture_id={capture_id}")
                    continue

                if state.active():
                    secs = len(state.pcm) / max(1, state.sample_rate * 2)
                    print(
                        f"[conn {conn_id}] WARN start while active:"
                        f" prev_id={state.capture_id} bytes={len(state.pcm)} sec={secs:.2f} -> reset"
                    )

                now = now_ms()
                _prune_pending_captures(now)
                state.reset()
                state.capture_id = capture_id
                state.sample_rate = sample_rate if sample_rate > 0 else 16000
                state.started_ms = now
                resumed = _pending_captures.pop(capture_id, None)
                resumed_bytes = 0
                if resumed is not None and resumed.sample_rate == state.sample_rate and resumed.pcm:
                    state.pcm.extend(resumed.pcm)
                    resumed_bytes = len(resumed.pcm)
                print(
                    f"[conn {conn_id}] [start] capture_id={state.capture_id} sr={state.sample_rate}"
                    + (f" resumed_bytes={resumed_bytes}" if resumed_bytes > 0 else "")
                )
                if state.sample_rate != 16000:
                    print(f"[conn {conn_id}] WARN vosk expects 16000Hz, got {state.sample_rate}Hz")
                vosk.start()
                if resumed_bytes > 0:
                    vosk.feed_pcm16(bytes(state.pcm))
                continue

            if msg_type == "end":
                end_capture_id = int(obj.get("capture_id", 0))
                if not state.active():
                    print(f"[conn {conn_id}] WARN end without active capture id={end_capture_id}")
                    continue

                if end_capture_id not in (0, state.capture_id):
                    print(
                        f"[conn {conn_id}] WARN end capture mismatch:"
                        f" got={end_capture_id} active={state.capture_id} (continuing with active)"
                    )

                active_id = state.capture_id
                pcm_bytes = bytes(state.pcm)
                seconds = len(pcm_bytes) / max(1, state.sample_rate * 2)
                d = pcm_diag(pcm_bytes)
                print(
                    f"[conn {conn_id}] [end] capture_id={active_id} bytes={len(pcm_bytes)}"
                    f" sec={seconds:.2f} "
                    f"rms={d['rms']:.1f} peak={int(d['peak'])} dc={d['dc']:.1f} clip={d['clip_ratio']:.3f}"
                )
                if d["rms"] < 300.0:
                    print(f"[conn {conn_id}] [diag] likely silence/too-quiet capture (rms<{300})")
                elif d["clip_ratio"] > 0.02:
                    print(f"[conn {conn_id}] [diag] signal clipping detected (clip_ratio>{0.02})")

                vres = vosk.end()
                text = vres.text
                word_diag = ", ".join([f"{w.word}:{w.confidence:.2f}" for w in vres.words]) if vres.words else "-"
                question_form = "open"
                prediction_polarity = "neutral"
                if text:
                    top: List[str] = []
                    top_scores: List[float] = []
                    margin = 0.0
                    polarity_conf = 0.0
                    polarity_diag = {"positive": 0.0, "negative": 0.0, "delta": 0.0}
                    best_hits: list[str] = []
                    if intent_keywords:
                        intent, conf, debug = infer_intent_from_text(text, intent_keywords)
                        top = debug.get("top", [])
                        top_scores = debug.get("top_scores", [])
                        margin = debug.get("margin", 0.0)
                        hit_map = debug.get("hits", {})
                        if isinstance(hit_map, dict):
                            if intent != "unknown":
                                best_hits = hit_map.get(intent, [])
                            elif top and isinstance(top[0], str):
                                best_hits = hit_map.get(top[0], [])
                        yes_no_score = 0.0
                        yes_no_hits = 0
                        scores = debug.get("scores", {})
                        if isinstance(scores, dict):
                            try:
                                yes_no_score = float(scores.get("yes_no", 0.0))
                            except Exception:
                                yes_no_score = 0.0
                        if isinstance(hit_map, dict):
                            raw_hits = hit_map.get("yes_no", [])
                            if isinstance(raw_hits, list):
                                yes_no_hits = len(raw_hits)
                        if oracle_bank is not None:
                            question_form = oracle_bank.detect_question_form(
                                text,
                                yes_no_score=yes_no_score,
                                yes_no_hits=yes_no_hits,
                            )
                            if oracle_bank.detect_timing_intent(text, current_intent=intent):
                                if intent in ("unknown", "future", "yes_no", "time"):
                                    intent = "time"
                                    conf = max(conf, 0.62)
                            prediction_polarity, polarity_conf, polarity_diag = oracle_bank.detect_prediction_polarity(
                                text,
                                question_form=question_form,
                            )
                    else:
                        intent = "unknown"
                        conf = 0.0

                    if reasoner is not None and intent not in ("forbidden", "joke"):
                        reasoned = await reasoner.infer(
                            ReasonerInput(
                                text=text,
                                intent_hint=intent,
                                confidence_hint=conf,
                                form_hint=question_form,
                                polarity_hint=prediction_polarity,
                                top_intents=[str(x) for x in top[:4]],
                                top_scores=[float(x) for x in top_scores[:4]],
                                margin=float(margin),
                                hits=[str(x) for x in best_hits[:8]],
                            )
                        )
                        if reasoned is not None:
                            intent = reasoned.intent or intent
                            question_form = reasoned.form or question_form
                            prediction_polarity = reasoned.polarity or prediction_polarity
                            conf = max(conf, reasoned.confidence)
                            print(
                                f"[conn {conn_id}] [reasoner] provider={reasoned.provider} "
                                f"intent={reasoned.intent} form={reasoned.form} "
                                f"polarity={reasoned.polarity} conf={reasoned.confidence:.3f}"
                            )

                    print(
                        f"[conn {conn_id}] [vosk] text={text!r} words=[{word_diag}] "
                        f"intent={intent} conf={conf:.3f} top={top} "
                        f"top_scores={top_scores} margin={margin:.1f} "
                        f"form={question_form} polarity={prediction_polarity}:{polarity_conf:.2f} "
                        f"pol={polarity_diag} hits={best_hits[:8]}"
                    )
                else:
                    intent = "unknown"
                    conf = 0.0
                    question_form = "open"
                    prediction_polarity = "neutral"
                    print(
                        f"[conn {conn_id}] [vosk] text={text!r} words=[{word_diag}] "
                        f"avg_conf={vres.confidence:.3f} -> intent=unknown"
                    )
                try:
                    await send_result(ws, active_id, intent, conf)
                    print(f"[conn {conn_id}] [result] id={active_id} intent={intent} conf={conf:.3f}")
                except Exception as exc:
                    print(f"[conn {conn_id}] result send failed: {exc}")

                last_intent = intent
                last_question_form = question_form
                last_prediction_polarity = prediction_polarity
                last_transcript = text or ""
                last_confidence = float(conf)
                state.reset()
                continue

            print(f"[conn {conn_id}] ignore type={msg_type!r}")

    except ConnectionClosed as exc:
        code = getattr(exc, "code", None)
        reason = getattr(exc, "reason", "")
        print(f"[conn {conn_id}] closed by peer code={code} reason={reason!r}")
    except Exception as exc:
        print(f"[conn {conn_id}] closed with error: {exc}")
    finally:
        if state.active():
            secs = len(state.pcm) / max(1, state.sample_rate * 2)
            print(
                f"[conn {conn_id}] close with active capture:"
                f" id={state.capture_id} bytes={len(state.pcm)} sec={secs:.2f}"
            )
            if len(state.pcm) > 0:
                _prune_pending_captures(now_ms())
                _pending_captures[state.capture_id] = PendingCapture(
                    capture_id=state.capture_id,
                    sample_rate=state.sample_rate,
                    pcm=bytes(state.pcm),
                    stored_ms=now_ms(),
                )
                print(
                    f"[conn {conn_id}] pending capture stored:"
                    f" id={state.capture_id} bytes={len(state.pcm)}"
                )
        print(f"[conn {conn_id}] closed")


async def main_async(args: argparse.Namespace) -> None:
    vosk_model = str(Path(args.vosk_model).expanduser())
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
    backend_probe = await validate_tts_backend(tts_cfg)
    if bool(args.tts_warmup):
        try:
            await warmup_tts_runtime(
                tts_cfg=tts_cfg,
                passes=max(1, int(args.tts_warmup_passes)),
                sample_rate=max(8000, min(48000, int(args.tts_warmup_sample_rate))),
                text=str(args.tts_warmup_text),
            )
        except Exception as exc:
            print(f"WARN tts warmup failed: {exc}")

    oracle_texts_dir = Path(args.oracle_texts_dir).expanduser().resolve()
    oracle_bank: Optional[OracleTextBank] = None
    oracle_seed = args.oracle_seed if int(args.oracle_seed) >= 0 else None
    if oracle_texts_dir.exists():
        try:
            oracle_bank = OracleTextBank.load_from_dir(oracle_texts_dir, seed=oracle_seed)
            print(
                f"oracle_texts loaded: dir={oracle_texts_dir} "
                f"domains={len(oracle_bank.domains)} service_sections={len(oracle_bank.services)}"
            )
        except Exception as exc:
            print(f"WARN failed to load oracle texts from {oracle_texts_dir}: {exc}")
            oracle_bank = None
    else:
        print(f"WARN oracle texts dir does not exist: {oracle_texts_dir}")

    stress_overrides: Optional[StressOverrides] = None
    stress_map_path = Path(args.tts_stress_map).expanduser().resolve()
    if not bool(args.tts_stress_disable):
        if stress_map_path.exists():
            try:
                stress_overrides = load_stress_overrides(stress_map_path)
            except Exception as exc:
                print(f"WARN failed to load stress map {stress_map_path}: {exc}")
                stress_overrides = None
        else:
            print(f"WARN stress map does not exist: {stress_map_path}")

    intent_map_path = Path(args.intent_map).expanduser().resolve() if args.intent_map else default_intent_map_path()
    intent_keywords: Any = {}
    if intent_map_path.exists():
        try:
            intent_keywords = load_intent_keywords(intent_map_path)
        except Exception as exc:
            print(f"WARN failed to load intent map {intent_map_path}: {exc}")
            intent_keywords = {}
    else:
        print(f"WARN intent map does not exist: {intent_map_path}")

    reasoner, reasoner_probe = build_reasoner(
        ReasonerConfig(
            backend=str(args.reasoner_backend).strip().lower(),
            timeout_ms=int(args.reasoner_timeout_ms),
            temperature=float(args.reasoner_temperature),
            max_output_tokens=int(args.reasoner_max_output_tokens),
            local_endpoint=str(args.reasoner_local_endpoint).strip(),
            local_model=str(args.reasoner_local_model).strip(),
            openai_endpoint=str(args.reasoner_openai_endpoint).strip(),
            openai_model=str(args.reasoner_openai_model).strip(),
            openai_api_key=str(args.reasoner_openai_api_key).strip(),
        )
    )
    oracle_llm, oracle_llm_probe = build_oracle_llm(
        OracleLlmConfig(
            backend=str(args.oracle_llm_backend).strip().lower(),
            timeout_ms=int(args.oracle_llm_timeout_ms),
            temperature=float(args.oracle_llm_temperature),
            max_output_tokens=int(args.oracle_llm_max_output_tokens),
            local_endpoint=str(args.oracle_llm_local_endpoint).strip(),
            local_model=str(args.oracle_llm_local_model).strip(),
            local_api_key=str(args.oracle_llm_local_api_key).strip(),
        )
    )

    effective_tts_pacing = str(args.tts_pacing)

    async def _handler(ws: Any) -> None:
        path = getattr(ws, "path", "/mic")
        if path != "/mic":
            print(f"[conn {id(ws):x}] reject path={path}")
            await ws.close(code=1008, reason="path must be /mic")
            return
        await handle_client(
            ws,
            vosk_model=vosk_model,
            intent_keywords=intent_keywords,
            reasoner=reasoner,
            tts_cfg=tts_cfg,
            tts_chunk_ms=args.tts_chunk_ms,
            tts_segment_max_chars=args.tts_segment_max_chars,
            tts_phrase_pause_ms=args.tts_phrase_pause_ms,
            tts_pacing=effective_tts_pacing,
            tts_pacing_lead_ms=args.tts_pacing_lead_ms,
            tts_chunk_min_bytes=args.tts_chunk_min_bytes,
            oracle_bank=oracle_bank,
            oracle_llm=oracle_llm,
            stress_overrides=stress_overrides,
        )

    print(
        f"Starting mic WS server on {args.host}:{args.port} path=/mic "
        f"backend=vosk vosk_model={vosk_model} intent_map={intent_map_path} "
        f"tts_backend={tts_cfg.backend} backend_probe={backend_probe} tts_chunk={args.tts_chunk_ms}ms "
        f"tts_chunk_min_bytes={args.tts_chunk_min_bytes} "
        f"tts_segment_max_chars={args.tts_segment_max_chars} "
        f"tts_phrase_pause_ms={args.tts_phrase_pause_ms} "
        f"tts_pacing_lead_ms={args.tts_pacing_lead_ms} "
        f"tts_pacing={effective_tts_pacing} "
        f"tts_warmup={'on' if args.tts_warmup else 'off'} "
        f"tts_warmup_passes={args.tts_warmup_passes} "
        f"tts_warmup_sr={args.tts_warmup_sample_rate} "
        f"piper_default_model={tts_cfg.piper_default_model or '<auto>'} "
        f"silero={tts_cfg.silero_language}/{tts_cfg.silero_speaker_model}:{tts_cfg.silero_speaker or '<auto>'} "
        f"yandex_voice={tts_cfg.yandex_voice or '<unset>'} "
        f"oracle_texts={oracle_texts_dir} "
        f"stress_map={stress_map_path if stress_overrides is not None else 'disabled'} "
        f"stress_words={(len(stress_overrides.words) if stress_overrides else 0)} "
        f"stress_phrases={(len(stress_overrides.phrases) if stress_overrides else 0)} "
        f"reasoner={reasoner_probe} "
        f"oracle_llm={oracle_llm_probe} "
        f"piper[length={tts_cfg.piper_length_scale:.2f} pitch={tts_cfg.piper_pitch_scale:.2f} tempo={tts_cfg.tts_tempo_scale:.2f}] "
        f"fx[preset={fx['preset']} echo_mix={tts_cfg.tts_echo_mix:.2f} echo_delay={tts_cfg.tts_echo_delay_ms} "
        f"echo_fb={tts_cfg.tts_echo_feedback:.2f} reverb_mix={tts_cfg.tts_reverb_mix:.2f} "
        f"reverb_room={tts_cfg.tts_reverb_room_scale:.2f} reverb_damp={tts_cfg.tts_reverb_damp:.2f}]"
    )
    async with serve(
        _handler,
        args.host,
        args.port,
        max_size=None,
        max_queue=32,
        ping_interval=20,
        ping_timeout=20,
    ):
        await asyncio.Future()


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Orb mic websocket server for Raspberry Pi")
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--vosk-model", default=os.getenv("ORB_WS_VOSK_MODEL", "~/orb_ws/models/vosk-model-small-ru-0.22"))
    p.add_argument("--intent-map", default=os.getenv("ORB_WS_INTENT_MAP", ""))
    p.add_argument("--reasoner-backend", default=os.getenv("ORB_REASONER_BACKEND", "none"), choices=["none", "local", "openai"])
    p.add_argument("--reasoner-timeout-ms", type=int, default=int(os.getenv("ORB_REASONER_TIMEOUT_MS", "1800")))
    p.add_argument("--reasoner-temperature", type=float, default=float(os.getenv("ORB_REASONER_TEMPERATURE", "0.1")))
    p.add_argument("--reasoner-max-output-tokens", type=int, default=int(os.getenv("ORB_REASONER_MAX_OUTPUT_TOKENS", "120")))
    p.add_argument("--reasoner-local-endpoint", default=os.getenv("ORB_REASONER_LOCAL_ENDPOINT", "http://127.0.0.1:8080/v1/chat/completions"))
    p.add_argument("--reasoner-local-model", default=os.getenv("ORB_REASONER_LOCAL_MODEL", "qwen2.5-1.5b-instruct-q4_k_m"))
    p.add_argument("--reasoner-openai-endpoint", default=os.getenv("ORB_REASONER_OPENAI_ENDPOINT", "https://api.openai.com/v1/chat/completions"))
    p.add_argument("--reasoner-openai-model", default=os.getenv("ORB_REASONER_OPENAI_MODEL", "gpt-5-mini"))
    p.add_argument("--reasoner-openai-api-key", default=os.getenv("ORB_REASONER_OPENAI_API_KEY", ""))
    p.add_argument("--oracle-llm-backend", default=os.getenv("ORB_ORACLE_LLM_BACKEND", "none"), choices=["none", "local"])
    p.add_argument("--oracle-llm-timeout-ms", type=int, default=int(os.getenv("ORB_ORACLE_LLM_TIMEOUT_MS", "2200")))
    p.add_argument("--oracle-llm-temperature", type=float, default=float(os.getenv("ORB_ORACLE_LLM_TEMPERATURE", "0.2")))
    p.add_argument("--oracle-llm-max-output-tokens", type=int, default=int(os.getenv("ORB_ORACLE_LLM_MAX_OUTPUT_TOKENS", "96")))
    p.add_argument("--oracle-llm-local-endpoint", default=os.getenv("ORB_ORACLE_LLM_LOCAL_ENDPOINT", "http://127.0.0.1:8080/v1/chat/completions"))
    p.add_argument("--oracle-llm-local-model", default=os.getenv("ORB_ORACLE_LLM_LOCAL_MODEL", "qwen2.5-3b-instruct-q4_k_m"))
    p.add_argument("--oracle-llm-local-api-key", default=os.getenv("ORB_ORACLE_LLM_LOCAL_API_KEY", ""))
    p.add_argument("--tts-backend", default=os.getenv("ORB_WS_TTS_BACKEND", "silero"), choices=["none", "piper", "silero", "yandex", "tone"])
    p.add_argument("--tts-pacing", default=os.getenv("ORB_WS_TTS_PACING", "realtime"), choices=["realtime", "adaptive", "none"])
    p.add_argument("--tts-chunk-ms", type=int, default=int(os.getenv("ORB_WS_TTS_CHUNK_MS", "20")))
    p.add_argument("--tts-chunk-min-bytes", type=int, default=int(os.getenv("ORB_WS_TTS_CHUNK_MIN_BYTES", "4096")))
    p.add_argument("--tts-segment-max-chars", type=int, default=int(os.getenv("ORB_WS_TTS_SEGMENT_MAX_CHARS", "260")))
    p.add_argument("--tts-phrase-pause-ms", type=int, default=int(os.getenv("ORB_WS_TTS_PHRASE_PAUSE_MS", "1500")))
    p.add_argument("--tts-pacing-lead-ms", type=int, default=int(os.getenv("ORB_WS_TTS_PACING_LEAD_MS", "0")))
    p.add_argument("--tts-warmup", dest="tts_warmup", action="store_true", default=(os.getenv("ORB_WS_TTS_WARMUP", "1") != "0"))
    p.add_argument("--no-tts-warmup", dest="tts_warmup", action="store_false")
    p.add_argument("--tts-warmup-passes", type=int, default=int(os.getenv("ORB_WS_TTS_WARMUP_PASSES", "2")))
    p.add_argument("--tts-warmup-sample-rate", type=int, default=int(os.getenv("ORB_WS_TTS_WARMUP_SAMPLE_RATE", "44100")))
    p.add_argument("--tts-warmup-text", default=os.getenv("ORB_WS_TTS_WARMUP_TEXT", "Проверка голоса. Система готова."))
    p.add_argument("--oracle-texts-dir", default=os.getenv("ORB_WS_ORACLE_TEXTS_DIR", str(default_oracle_texts_dir())))
    p.add_argument("--oracle-seed", type=int, default=int(os.getenv("ORB_WS_ORACLE_SEED", "-1")))
    p.add_argument("--tts-stress-map", default=os.getenv("ORB_WS_TTS_STRESS_MAP", str(default_stress_map_path())))
    p.add_argument("--tts-stress-disable", action="store_true", help="Disable stress/accents override map")
    p.add_argument("--piper-bin", default=os.getenv("ORB_WS_PIPER_BIN", "~/orb_ws/piper/bin/piper"))
    p.add_argument("--piper-model-dir", default=os.getenv("ORB_WS_PIPER_MODEL_DIR", "~/orb_ws/models/piper"))
    p.add_argument("--piper-default-model", default=os.getenv("ORB_WS_PIPER_DEFAULT_MODEL", "ru_RU-ruslan-medium.onnx"))
    p.add_argument("--piper-default-config", default=os.getenv("ORB_WS_PIPER_DEFAULT_CONFIG", ""))
    p.add_argument("--piper-espeak-data-dir", default=os.getenv("ORB_WS_PIPER_ESPEAK_DATA_DIR", "~/orb_ws/piper/espeak-ng-data"))
    p.add_argument("--piper-length-scale", type=float, default=float(os.getenv("ORB_WS_PIPER_LENGTH_SCALE", "1.38")))
    p.add_argument("--piper-noise-scale", type=float, default=float(os.getenv("ORB_WS_PIPER_NOISE_SCALE", "0.28")))
    p.add_argument("--piper-noise-w", type=float, default=float(os.getenv("ORB_WS_PIPER_NOISE_W", "0.35")))
    p.add_argument("--piper-sentence-silence-s", type=float, default=float(os.getenv("ORB_WS_PIPER_SENTENCE_SILENCE_S", "0.34")))
    p.add_argument("--piper-pitch-scale", type=float, default=float(os.getenv("ORB_WS_PIPER_PITCH_SCALE", "0.98")))
    p.add_argument("--tts-tempo-scale", type=float, default=float(os.getenv("ORB_WS_TTS_TEMPO_SCALE", "0.92")))
    p.add_argument("--silero-repo-or-dir", default=os.getenv("ORB_WS_SILERO_REPO_OR_DIR", "snakers4/silero-models"))
    p.add_argument("--silero-language", default=os.getenv("ORB_WS_SILERO_LANGUAGE", "ru"))
    p.add_argument("--silero-speaker-model", default=os.getenv("ORB_WS_SILERO_SPEAKER_MODEL", "v4_ru"))
    p.add_argument("--silero-speaker", default=os.getenv("ORB_WS_SILERO_SPEAKER", "xenia"))
    p.add_argument("--silero-sample-rate", type=int, default=int(os.getenv("ORB_WS_SILERO_SAMPLE_RATE", "48000")))
    p.add_argument("--silero-device", default=os.getenv("ORB_WS_SILERO_DEVICE", "cpu"))
    p.add_argument("--silero-put-accent", action="store_true", default=(os.getenv("ORB_WS_SILERO_PUT_ACCENT", "1") != "0"))
    p.add_argument("--silero-no-put-accent", dest="silero_put_accent", action="store_false")
    p.add_argument("--silero-put-yo", action="store_true", default=(os.getenv("ORB_WS_SILERO_PUT_YO", "1") != "0"))
    p.add_argument("--silero-no-put-yo", dest="silero_put_yo", action="store_false")
    p.add_argument("--silero-num-threads", type=int, default=int(os.getenv("ORB_WS_SILERO_NUM_THREADS", "1")))
    p.add_argument("--yandex-endpoint", default=os.getenv("ORB_WS_YANDEX_ENDPOINT", "https://tts.api.cloud.yandex.net/speech/v1/tts:synthesize"))
    p.add_argument("--yandex-api-key", default=os.getenv("ORB_WS_YANDEX_API_KEY", ""))
    p.add_argument("--yandex-iam-token", default=os.getenv("ORB_WS_YANDEX_IAM_TOKEN", ""))
    p.add_argument("--yandex-folder-id", default=os.getenv("ORB_WS_YANDEX_FOLDER_ID", ""))
    p.add_argument("--yandex-lang", default=os.getenv("ORB_WS_YANDEX_LANG", "ru-RU"))
    p.add_argument("--yandex-voice", default=os.getenv("ORB_WS_YANDEX_VOICE", "ermil"))
    p.add_argument("--yandex-speed", type=float, default=float(os.getenv("ORB_WS_YANDEX_SPEED", "1.0")))
    p.add_argument("--yandex-emotion", default=os.getenv("ORB_WS_YANDEX_EMOTION", ""))
    p.add_argument("--yandex-timeout-s", type=float, default=float(os.getenv("ORB_WS_YANDEX_TIMEOUT_S", "20.0")))
    p.add_argument("--tts-fx-preset", default=os.getenv("ORB_WS_TTS_FX_PRESET", "mystic"), choices=["off", "soft", "mystic", "deep"])
    p.add_argument("--tts-echo-mix", type=float, default=None, help="Optional override for echo mix [0..1]")
    p.add_argument("--tts-echo-delay-ms", type=int, default=None, help="Optional override for echo delay in ms")
    p.add_argument("--tts-echo-feedback", type=float, default=None, help="Optional override for echo feedback [0..1]")
    p.add_argument("--tts-reverb-mix", type=float, default=None, help="Optional override for reverb mix [0..1]")
    p.add_argument("--tts-reverb-room-scale", type=float, default=None, help="Optional override for reverb room scale")
    p.add_argument("--tts-reverb-damp", type=float, default=None, help="Optional override for reverb damping [0..1]")
    p.add_argument("--tts-tail-fade-ms", type=int, default=int(os.getenv("ORB_WS_TTS_TAIL_FADE_MS", "20")))
    p.add_argument("--tts-tail-silence-ms", type=int, default=int(os.getenv("ORB_WS_TTS_TAIL_SILENCE_MS", "120")))
    p.add_argument("--tts-tone-hz", type=float, default=float(os.getenv("ORB_WS_TTS_TONE_HZ", "280.0")))
    p.add_argument("--tts-tone-ms", type=int, default=int(os.getenv("ORB_WS_TTS_TONE_MS", "1400")))
    p.add_argument("--tts-tone-gain", type=float, default=float(os.getenv("ORB_WS_TTS_TONE_GAIN", "0.35")))
    return p.parse_args()


def main() -> int:
    args = parse_args()
    asyncio.run(main_async(args))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
