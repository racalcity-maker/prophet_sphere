from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple

from .base import OracleLlmBackend
from .http_llm import HttpOracleLlm, HttpOracleLlmConfig


@dataclass
class OracleLlmConfig:
    backend: str = "none"  # none | local
    timeout_ms: int = 2200
    temperature: float = 0.2
    max_output_tokens: int = 96
    local_endpoint: str = "http://127.0.0.1:8080/v1/chat/completions"
    local_model: str = "qwen2.5-3b-instruct-q4_k_m"
    local_api_key: str = ""


def build_oracle_llm(cfg: OracleLlmConfig) -> Tuple[Optional[OracleLlmBackend], str]:
    backend = (cfg.backend or "none").strip().lower()
    if backend == "none":
        return None, "disabled"

    if backend == "local":
        llm = HttpOracleLlm(
            HttpOracleLlmConfig(
                endpoint=cfg.local_endpoint.strip(),
                model=cfg.local_model.strip(),
                timeout_ms=max(300, int(cfg.timeout_ms)),
                temperature=float(cfg.temperature),
                max_output_tokens=max(64, int(cfg.max_output_tokens)),
                api_key=cfg.local_api_key.strip(),
                provider="oracle_local_llm",
            )
        )
        return llm, f"local endpoint={cfg.local_endpoint} model={cfg.local_model}"

    return None, f"disabled: unknown backend={backend}"
