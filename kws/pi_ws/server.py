#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import numpy as np

try:
    # Preferred package imports (python -m kws.pi_ws.server)
    from .asr_vosk import make_vosk_session
    from .dialog_session import handle_client as dialog_handle_client
    from .intent_from_text import VALID_INTENTS, infer_intent_from_text, load_intent_keywords
    from .oracle_llm import OracleLlmConfig, OracleLlmInput, build_oracle_llm
    from .reasoner import ReasonerConfig, ReasonerInput, build_reasoner
    from .runtime_tts import RuntimeTtsState, RuntimeTtsStore, _build_runtime_tts_state, warmup_tts_runtime
    from .text_bank import OracleTextBank
    from .tts_control_api import start_tts_control_server
    from .tts_synth import validate_tts_backend
except Exception:  # pragma: no cover
    # Fallback when running as a plain script file
    from asr_vosk import make_vosk_session  # type: ignore
    from dialog_session import handle_client as dialog_handle_client  # type: ignore
    from intent_from_text import VALID_INTENTS, infer_intent_from_text, load_intent_keywords  # type: ignore
    from oracle_llm import OracleLlmConfig, OracleLlmInput, build_oracle_llm  # type: ignore
    from reasoner import ReasonerConfig, ReasonerInput, build_reasoner  # type: ignore
    from runtime_tts import RuntimeTtsState, RuntimeTtsStore, _build_runtime_tts_state, warmup_tts_runtime  # type: ignore
    from text_bank import OracleTextBank  # type: ignore
    from tts_control_api import start_tts_control_server  # type: ignore
    from tts_synth import validate_tts_backend  # type: ignore

try:
    from websockets.asyncio.server import serve
except Exception:  # pragma: no cover
    from websockets.server import serve  # type: ignore

try:
    from websockets.exceptions import ConnectionClosed
except Exception:  # pragma: no cover
    ConnectionClosed = Exception  # type: ignore

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
) -> None:
    await dialog_handle_client(
        ws,
        vosk_model=vosk_model,
        intent_keywords=intent_keywords,
        reasoner=reasoner,
        runtime_tts_store=runtime_tts_store,
        oracle_bank=oracle_bank,
        oracle_llm=oracle_llm,
        stress_overrides=stress_overrides,
        parse_json_text_fn=parse_json_text,
        resolve_tts_text_fn=resolve_tts_text,
        apply_stress_overrides_fn=apply_stress_overrides,
        strip_tts_stress_markers_fn=strip_tts_stress_markers,
        split_tts_text_fn=split_tts_text,
        pcm_diag_fn=pcm_diag,
        now_ms_fn=now_ms,
    )


async def main_async(args: argparse.Namespace) -> None:
    vosk_model = str(Path(args.vosk_model).expanduser())
    tts_runtime = _build_runtime_tts_state(args)
    tts_config_file = Path(args.tts_config_file).expanduser().resolve()
    runtime_tts_store = RuntimeTtsStore(tts_runtime, tts_config_file)
    if bool(args.tts_config_autoload):
        try:
            runtime_tts_store.load()
            print(f"tts runtime config loaded: {tts_config_file}")
        except FileNotFoundError:
            print(f"tts runtime config not found, using CLI defaults: {tts_config_file}")
        except Exception as exc:
            print(f"WARN failed to load tts runtime config {tts_config_file}: {exc}")

    tts_snapshot = runtime_tts_store.snapshot()
    tts_cfg = tts_snapshot.tts_cfg
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
    oracle_texts_format = str(getattr(args, "oracle_texts_format", "auto")).strip().lower()
    if oracle_texts_format not in ("auto", "legacy", "v2"):
        oracle_texts_format = "auto"
    oracle_texts_legacy_compat = bool(getattr(args, "oracle_texts_legacy_compat", False))
    oracle_bank: Optional[OracleTextBank] = None
    oracle_seed = args.oracle_seed if int(args.oracle_seed) >= 0 else None
    if oracle_texts_dir.exists():
        try:
            oracle_bank = OracleTextBank.load_from_dir(
                oracle_texts_dir,
                seed=oracle_seed,
                format_mode=oracle_texts_format,
                legacy_compat=oracle_texts_legacy_compat,
            )
            subintent_domains = len([k for k, v in oracle_bank.subintent_rules.items() if v])
            subintent_total = sum(len(v) for v in oracle_bank.subintent_rules.values())
            print(
                f"oracle_texts loaded: dir={oracle_texts_dir} "
                f"format={oracle_texts_format} legacy_compat={'on' if oracle_texts_legacy_compat else 'off'} "
                f"domains={len(oracle_bank.domains)} service_sections={len(oracle_bank.services)} "
                f"subintent_domains={subintent_domains} subintents={subintent_total}"
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
            runtime_tts_store=runtime_tts_store,
            oracle_bank=oracle_bank,
            oracle_llm=oracle_llm,
            stress_overrides=stress_overrides,
        )

    control_server = await start_tts_control_server(
        host=str(args.control_host).strip(),
        port=int(args.control_port),
        runtime_store=runtime_tts_store,
        allow_origin=str(args.control_allow_origin).strip() or "*",
        auth_token=str(args.control_token).strip(),
    )
    print(
        f"tts control API on http://{args.control_host}:{args.control_port} "
        f"file={tts_config_file} autoload={'on' if args.tts_config_autoload else 'off'} "
        f"token={'set' if str(args.control_token).strip() else 'off'}"
    )

    tts_snapshot = runtime_tts_store.snapshot()
    tts_cfg = tts_snapshot.tts_cfg

    print(
        f"Starting mic WS server on {args.host}:{args.port} path=/mic "
        f"backend=vosk vosk_model={vosk_model} intent_map={intent_map_path} "
        f"tts_backend={tts_cfg.backend} backend_probe={backend_probe} tts_chunk={tts_snapshot.tts_chunk_ms}ms "
        f"tts_chunk_min_bytes={tts_snapshot.tts_chunk_min_bytes} "
        f"tts_segment_max_chars={tts_snapshot.tts_segment_max_chars} "
        f"tts_phrase_pause_ms={tts_snapshot.tts_phrase_pause_ms} "
        f"tts_pacing_lead_ms={tts_snapshot.tts_pacing_lead_ms} "
        f"tts_pacing={tts_snapshot.tts_pacing} "
        f"tts_warmup={'on' if args.tts_warmup else 'off'} "
        f"tts_warmup_passes={args.tts_warmup_passes} "
        f"tts_warmup_sr={args.tts_warmup_sample_rate} "
        f"piper_default_model={tts_cfg.piper_default_model or '<auto>'} "
        f"silero={tts_cfg.silero_language}/{tts_cfg.silero_speaker_model}:{tts_cfg.silero_speaker or '<auto>'} "
        f"yandex_voice={tts_cfg.yandex_voice or '<unset>'} "
        f"oracle_texts={oracle_texts_dir} format={oracle_texts_format} "
        f"legacy_compat={'on' if oracle_texts_legacy_compat else 'off'} "
        f"stress_map={stress_map_path if stress_overrides is not None else 'disabled'} "
        f"stress_words={(len(stress_overrides.words) if stress_overrides else 0)} "
        f"stress_phrases={(len(stress_overrides.phrases) if stress_overrides else 0)} "
        f"reasoner={reasoner_probe} "
        f"oracle_llm={oracle_llm_probe} "
        f"piper[length={tts_cfg.piper_length_scale:.2f} pitch={tts_cfg.piper_pitch_scale:.2f} tempo={tts_cfg.tts_tempo_scale:.2f}] "
        f"fx[preset={tts_snapshot.tts_fx_preset} echo_mix={tts_cfg.tts_echo_mix:.2f} echo_delay={tts_cfg.tts_echo_delay_ms} "
        f"echo_fb={tts_cfg.tts_echo_feedback:.2f} reverb_mix={tts_cfg.tts_reverb_mix:.2f} "
        f"reverb_room={tts_cfg.tts_reverb_room_scale:.2f} reverb_damp={tts_cfg.tts_reverb_damp:.2f}]"
    )
    try:
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
    finally:
        control_server.close()
        await control_server.wait_closed()


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
    p.add_argument(
        "--oracle-texts-format",
        default=os.getenv("ORB_WS_ORACLE_TEXTS_FORMAT", "auto"),
        choices=["auto", "legacy", "v2"],
        help="Oracle text format loader mode",
    )
    p.add_argument(
        "--oracle-texts-legacy-compat",
        action="store_true",
        default=(os.getenv("ORB_WS_ORACLE_TEXTS_LEGACY_COMPAT", "0") == "1"),
        help="In auto/v2 mode, merge legacy *.txt phrases as fallback",
    )
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
    p.add_argument("--control-host", default=os.getenv("ORB_WS_CONTROL_HOST", "0.0.0.0"))
    p.add_argument("--control-port", type=int, default=int(os.getenv("ORB_WS_CONTROL_PORT", "8766")))
    p.add_argument("--control-allow-origin", default=os.getenv("ORB_WS_CONTROL_ALLOW_ORIGIN", "*"))
    p.add_argument("--control-token", default=os.getenv("ORB_WS_CONTROL_TOKEN", ""))
    p.add_argument(
        "--tts-config-file",
        default=os.getenv("ORB_WS_TTS_CONFIG_FILE", str(Path("~/orb_ws/kws/pi_ws/tts_runtime_config.json").expanduser())),
    )
    p.add_argument("--tts-config-autoload", dest="tts_config_autoload", action="store_true", default=(os.getenv("ORB_WS_TTS_CONFIG_AUTOLOAD", "1") != "0"))
    p.add_argument("--no-tts-config-autoload", dest="tts_config_autoload", action="store_false")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    asyncio.run(main_async(args))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
