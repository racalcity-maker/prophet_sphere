from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple

from .base import ReasonerBackend
from .http_llm import HttpLlmReasoner, HttpLlmReasonerConfig


@dataclass
class ReasonerConfig:
    backend: str = "none"  # none | local | openai
    timeout_ms: int = 1800
    temperature: float = 0.1
    max_output_tokens: int = 120
    local_endpoint: str = "http://127.0.0.1:8080/v1/chat/completions"
    local_model: str = "qwen2.5-1.5b-instruct-q4_k_m"
    openai_endpoint: str = "https://api.openai.com/v1/chat/completions"
    openai_model: str = "gpt-5-mini"
    openai_api_key: str = ""


def build_reasoner(cfg: ReasonerConfig) -> Tuple[Optional[ReasonerBackend], str]:
    backend = (cfg.backend or "none").strip().lower()
    if backend == "none":
        return None, "disabled"

    if backend == "local":
        reasoner = HttpLlmReasoner(
            HttpLlmReasonerConfig(
                provider="local_llm",
                endpoint=cfg.local_endpoint.strip(),
                model=cfg.local_model.strip(),
                timeout_ms=max(300, int(cfg.timeout_ms)),
                temperature=float(cfg.temperature),
                max_output_tokens=max(32, int(cfg.max_output_tokens)),
                api_key="",
            )
        )
        return reasoner, f"local endpoint={cfg.local_endpoint} model={cfg.local_model}"

    if backend == "openai":
        api_key = cfg.openai_api_key.strip()
        if not api_key:
            return None, "openai disabled: missing api key"
        reasoner = HttpLlmReasoner(
            HttpLlmReasonerConfig(
                provider="openai",
                endpoint=cfg.openai_endpoint.strip(),
                model=cfg.openai_model.strip(),
                timeout_ms=max(300, int(cfg.timeout_ms)),
                temperature=float(cfg.temperature),
                max_output_tokens=max(32, int(cfg.max_output_tokens)),
                api_key=api_key,
            )
        )
        return reasoner, f"openai endpoint={cfg.openai_endpoint} model={cfg.openai_model}"

    return None, f"disabled: unknown backend={backend}"

