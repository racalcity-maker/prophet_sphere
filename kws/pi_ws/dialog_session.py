from __future__ import annotations

import asyncio
import json
import re
import time
import traceback
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
from websockets.exceptions import ConnectionClosed

try:
    from .asr_vosk import make_vosk_session
    from .intent_from_text import infer_intent_from_text
    from .reasoner import ReasonerInput
    from .tts_synth import synthesize_tts
except Exception:  # pragma: no cover
    from asr_vosk import make_vosk_session  # type: ignore
    from intent_from_text import infer_intent_from_text  # type: ignore
    from reasoner import ReasonerInput  # type: ignore
    from tts_synth import synthesize_tts  # type: ignore


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


@dataclass
class StreamPostFx:
    enabled: bool = True
    declick_threshold: int = 900
    declick_ramp_samples: int = 32
    soft_limit_threshold: int = 28000
    soft_limit_knee: int = 2600
    _last_sample: int = 0
    _has_last_sample: bool = False

    def reset(self) -> None:
        self._last_sample = 0
        self._has_last_sample = False

    def process(self, pcm_le16: bytes) -> bytes:
        if (not self.enabled) or (not pcm_le16):
            return pcm_le16
        s = np.frombuffer(pcm_le16, dtype="<i2")
        if s.size == 0:
            return pcm_le16

        out = s.astype(np.int32, copy=True)

        # Fast DC trim per chunk: removes low-frequency bias that often causes
        # onset artifacts after aggressive tempo/pitch transforms.
        dc = int(np.mean(out))
        if dc != 0:
            out -= dc

        # De-click at chunk boundary.
        if self._has_last_sample:
            first = int(out[0])
            jump = abs(first - self._last_sample)
            if jump >= self.declick_threshold:
                ramp_n = min(int(out.size), max(1, int(self.declick_ramp_samples)))
                prev = int(self._last_sample)
                head = out[:ramp_n].copy()
                idx = np.arange(1, ramp_n + 1, dtype=np.int32)
                out[:ramp_n] = prev + ((head - prev) * idx) // ramp_n

        # Soft limiter with a light knee: stable peak control with minimal CPU.
        thr = int(self.soft_limit_threshold)
        knee = max(1, int(self.soft_limit_knee))
        abs_out = np.abs(out)
        mask = abs_out > thr
        if np.any(mask):
            over = abs_out[mask] - thr
            comp = thr + ((over * knee) // (knee + over))
            signs = np.where(out[mask] < 0, -1, 1)
            out[mask] = comp * signs

        np.clip(out, -32768, 32767, out=out)
        self._last_sample = int(out[-1])
        self._has_last_sample = True
        return out.astype("<i2", copy=False).tobytes()

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
    last_subintent: str,
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
    if key == "__ORACLE_RETRY__":
        if oracle_bank is not None:
            retry = oracle_bank.pick_service_phrase(
                "retry",
                fallback="Я не расслышала вопрос. Скажи ещё раз.",
            )
            if retry:
                return retry, "oracle_bank_retry"
        return "Я не расслышала вопрос. Скажи ещё раз.", "oracle_retry_fallback"
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
                subintent=last_subintent,
                question_form=last_question_form,
                force_polarity=last_prediction_polarity,
                include_opening_phases=False,
                allow_thinking_phase=False,
            )
            if selected.text:
                source = (
                    f"oracle_bank domain={selected.domain} form={selected.question_form} "
                    f"prediction={selected.prediction_bucket} "
                    f"subintent={selected.subintent or '-'}"
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

@dataclass(frozen=True)
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


async def send_result(ws: Any, capture_id: int, intent: str, conf: float, *, subintent: str = "") -> None:
    payload = {
        "type": "result",
        "capture_id": int(capture_id),
        "intent": intent,
        "confidence": float(conf),
    }
    if subintent:
        payload["subintent"] = str(subintent)
    await ws.send(json.dumps(payload, ensure_ascii=False))


async def send_tts_control(ws: Any, *, ok: bool, error: str = "") -> None:
    payload: Dict[str, Any] = {"type": "tts_done" if ok else "tts_error"}
    if not ok and error:
        payload["error"] = error
    await ws.send(json.dumps(payload, ensure_ascii=False))

async def handle_client(
    ws: Any,
    *,
    vosk_model: str,
    intent_keywords: Any,
    reasoner: Any,
    runtime_tts_store: RuntimeTtsStore,
    oracle_bank: Optional[OracleTextBank],
    oracle_llm: Any,
    stress_overrides: Optional[StressOverrides],
    parse_json_text_fn: Any = None,
    resolve_tts_text_fn: Any = None,
    apply_stress_overrides_fn: Any = None,
    strip_tts_stress_markers_fn: Any = None,
    split_tts_text_fn: Any = None,
    pcm_diag_fn: Any = None,
    now_ms_fn: Any = None,
) -> None:
    if parse_json_text_fn is None:
        parse_json_text_fn = parse_json_text
    if resolve_tts_text_fn is None:
        resolve_tts_text_fn = resolve_tts_text
    if apply_stress_overrides_fn is None:
        apply_stress_overrides_fn = apply_stress_overrides
    if strip_tts_stress_markers_fn is None:
        strip_tts_stress_markers_fn = strip_tts_stress_markers
    if split_tts_text_fn is None:
        split_tts_text_fn = split_tts_text
    if pcm_diag_fn is None:
        pcm_diag_fn = pcm_diag
    if now_ms_fn is None:
        now_ms_fn = now_ms

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
    last_subintent = ""
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

            obj = parse_json_text_fn(message)
            if obj is None:
                print(f"[conn {conn_id}] bad json: {message[:120]}")
                continue

            msg_type = str(obj.get("type", "")).strip().lower()
            if msg_type == "tts":
                requested_text = str(obj.get("text", "")).strip()
                text, text_source = await resolve_tts_text_fn(
                    requested_text,
                    oracle_bank=oracle_bank,
                    oracle_llm=oracle_llm,
                    last_intent=last_intent,
                    last_subintent=last_subintent,
                    last_question_form=last_question_form,
                    last_prediction_polarity=last_prediction_polarity,
                    last_transcript=last_transcript,
                    last_confidence=last_confidence,
                )
                text = apply_stress_overrides_fn(text, stress_overrides)
                text = strip_tts_stress_markers_fn(text)
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
                runtime_tts = runtime_tts_store.snapshot()
                tts_cfg = runtime_tts.tts_cfg
                tts_chunk_ms = runtime_tts.tts_chunk_ms
                tts_segment_max_chars = runtime_tts.tts_segment_max_chars
                tts_phrase_pause_ms = runtime_tts.tts_phrase_pause_ms
                tts_pacing = runtime_tts.tts_pacing
                tts_pacing_lead_ms = runtime_tts.tts_pacing_lead_ms
                tts_chunk_min_bytes = runtime_tts.tts_chunk_min_bytes

                try:
                    print(
                        f"[conn {conn_id}] [tts] request source={text_source} "
                        f"intent={last_intent} subintent={last_subintent or '-'} "
                        f"form={last_question_form} polarity={last_prediction_polarity} "
                        f"chars={len(text)}"
                    )
                    if text_source == "plain_text":
                        preview = text.replace("\r", "\\r").replace("\n", "\\n")
                        if len(preview) > 220:
                            preview = preview[:220] + "..."
                        print(f"[conn {conn_id}] [tts] plain_text preview='{preview}'")
                    t0 = now_ms_fn()
                    req_t0 = time.perf_counter()
                    segments = split_tts_text_fn(text, tts_segment_max_chars)
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
                    last_sr = req_rate
                    last_chunk_bytes = max(256, int(tts_chunk_min_bytes))
                    last_backend = "unknown"
                    boundary_jump_threshold = 5000
                    boundary_jump_count = 0
                    boundary_jump_max = 0
                    boundary_log_budget = 3
                    prev_out_sample = None
                    stream_postfx = StreamPostFx(enabled=True)

                    def _track_boundary_jump(pcm_chunk: bytes, stage: str) -> None:
                        nonlocal prev_out_sample, boundary_jump_count, boundary_jump_max, boundary_log_budget
                        if len(pcm_chunk) < 2:
                            return
                        first_s = int.from_bytes(pcm_chunk[0:2], byteorder="little", signed=True)
                        last_s = int.from_bytes(pcm_chunk[-2:], byteorder="little", signed=True)
                        if prev_out_sample is not None:
                            jump = abs(first_s - prev_out_sample)
                            if jump > boundary_jump_max:
                                boundary_jump_max = jump
                            if jump >= boundary_jump_threshold:
                                boundary_jump_count += 1
                                if boundary_log_budget > 0:
                                    boundary_log_budget -= 1
                                    print(
                                        f"[conn {conn_id}] [tts] boundary_jump stage={stage} "
                                        f"jump={jump} prev={prev_out_sample} first={first_s}"
                                    )
                        prev_out_sample = last_s
                    print(f"[conn {conn_id}] [tts] stream_postfx=fast enabled={1 if stream_postfx.enabled else 0}")
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
                        last_sr = int(max(8000, min(48000, tts.sample_rate)))
                        last_chunk_bytes = int(max(256, chunk_bytes))
                        last_backend = str(tts.backend or "").strip().lower()
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
                                payload = b"\x00" * (nxt - pre_roll_sent)
                                payload_fx = stream_postfx.process(payload)
                                await ws.send(payload_fx)
                                _track_boundary_jump(payload_fx, "pre_roll")
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
                            payload = tts.pcm_le16[sent:nxt]
                            payload_fx = stream_postfx.process(payload)
                            await ws.send(payload_fx)
                            _track_boundary_jump(payload_fx, f"seg{seg_idx + 1}")
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
                                payload = b"\x00" * (nxt - pause_sent)
                                payload_fx = stream_postfx.process(payload)
                                await ws.send(payload_fx)
                                _track_boundary_jump(payload_fx, "phrase_pause")
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

                    # Add a short terminal zero pad so ESP side never holds a stale
                    # non-zero sample at stream stop boundary (especially visible on silero).
                    end_pad_ms = 48 if last_backend == "silero" else 24
                    end_pad_samples = max(1, int(last_sr * end_pad_ms / 1000.0))
                    end_pad_bytes_total = end_pad_samples * 2
                    end_pad_sent = 0
                    end_pad_t0 = time.perf_counter()
                    while end_pad_sent < end_pad_bytes_total:
                        nxt = min(end_pad_bytes_total, end_pad_sent + last_chunk_bytes)
                        payload = b"\x00" * (nxt - end_pad_sent)
                        payload_fx = stream_postfx.process(payload)
                        await ws.send(payload_fx)
                        _track_boundary_jump(payload_fx, "end_pad")
                        end_pad_sent = nxt
                        if tts_pacing in ("realtime", "adaptive"):
                            end_progress_s = (end_pad_sent / 2) / float(max(1, last_sr))
                            target_t = end_pad_t0 + end_progress_s
                            delay_s = target_t - time.perf_counter()
                            if delay_s > 0.0:
                                await asyncio.sleep(delay_s if tts_pacing == "realtime" else min(delay_s, 0.03))
                    total += end_pad_bytes_total
                    total_audio_ms += end_pad_ms
                    print(
                        f"[conn {conn_id}] [tts] end_pad_ms={end_pad_ms} "
                        f"audio_ms={end_pad_ms} backend={last_backend}"
                    )
                    await send_tts_control(ws, ok=True)
                    dt = now_ms_fn() - t0
                    rtf = (float(dt) / float(max(1, total_audio_ms))) if total_audio_ms > 0 else 0.0
                    print(
                        f"[conn {conn_id}] [tts] done segments={seg_count} bytes={total} "
                        f"send_ms={dt} audio_ms={total_audio_ms} pacing={tts_pacing} rtf={rtf:.3f} "
                        f"boundary_jumps={boundary_jump_count} boundary_jump_max={boundary_jump_max}"
                    )
                except Exception as exc:
                    err_text = str(exc).strip()
                    err = err_text if err_text else exc.__class__.__name__
                    await send_tts_control(ws, ok=False, error=err[:120])
                    print(f"[conn {conn_id}] [tts] error: {err} ({exc.__class__.__name__})")
                    traceback.print_exc()
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

                now = now_ms_fn()
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
                d = pcm_diag_fn(pcm_bytes)
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
                subintent = ""
                if text:
                    top: List[str] = []
                    top_scores: List[float] = []
                    margin = 0.0
                    polarity_conf = 0.0
                    polarity_diag = {"positive": 0.0, "negative": 0.0, "delta": 0.0}
                    best_hits: list[str] = []
                    subintent_conf = 0.0
                    subintent_debug: Dict[str, object] = {}
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

                    if oracle_bank is not None and intent not in ("unknown", "uncertain", "forbidden", "joke"):
                        subintent, subintent_conf, subintent_debug = oracle_bank.detect_subintent(
                            text,
                            intent=intent,
                        )

                    print(
                        f"[conn {conn_id}] [vosk] text={text!r} words=[{word_diag}] "
                        f"intent={intent} conf={conf:.3f} top={top} "
                        f"top_scores={top_scores} margin={margin:.1f} "
                        f"form={question_form} polarity={prediction_polarity}:{polarity_conf:.2f} "
                        f"subintent={subintent or '-'}:{subintent_conf:.2f} "
                        f"pol={polarity_diag} hits={best_hits[:8]} "
                        f"subhits={(subintent_debug.get('hits', {}).get(subintent, []) if subintent else [])[:6]}"
                    )
                else:
                    intent = "unknown"
                    conf = 0.0
                    subintent = ""
                    question_form = "open"
                    prediction_polarity = "neutral"
                    print(
                        f"[conn {conn_id}] [vosk] text={text!r} words=[{word_diag}] "
                        f"avg_conf={vres.confidence:.3f} -> intent=unknown"
                    )
                try:
                    await send_result(ws, active_id, intent, conf, subintent=subintent)
                    print(
                        f"[conn {conn_id}] [result] id={active_id} intent={intent} "
                        f"subintent={subintent or '-'} conf={conf:.3f}"
                    )
                except Exception as exc:
                    print(f"[conn {conn_id}] result send failed: {exc}")

                last_intent = intent
                last_subintent = subintent
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
                try:
                    _prune_pending_captures(now_ms_fn())
                    _pending_captures[state.capture_id] = PendingCapture(
                        capture_id=state.capture_id,
                        sample_rate=state.sample_rate,
                        pcm=bytes(state.pcm),
                        stored_ms=now_ms_fn(),
                    )
                    print(
                        f"[conn {conn_id}] pending capture stored:"
                        f" id={state.capture_id} bytes={len(state.pcm)}"
                    )
                except Exception as exc:
                    print(f"[conn {conn_id}] pending capture store failed: {exc}")
        print(f"[conn {conn_id}] closed")
