from __future__ import annotations

import asyncio
import json
import re
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Dict, Optional

from .contracts import ReasonerInput, ReasonerResult, sanitize_result

_JSON_RE = re.compile(r"\{.*\}", re.DOTALL)

_SYSTEM_PROMPT = (
    "Ты классификатор намерения для голосового оракула. "
    "Отвечай строго одним JSON-объектом без markdown. "
    "Поля JSON: intent, form, polarity, confidence. "
    "intent только из: unknown,love,future,choice,money,path,danger,inner_state,wish,past,time,place,uncertain,joke,forbidden. "
    "form только open или yes_no. "
    "polarity только positive,negative,neutral. "
    "confidence число 0..0.999. "
    "yes_no это только form, не intent."
)


@dataclass
class HttpLlmReasonerConfig:
    provider: str
    endpoint: str
    model: str
    timeout_ms: int = 1800
    temperature: float = 0.1
    max_output_tokens: int = 120
    api_key: str = ""


class HttpLlmReasoner:
    def __init__(self, cfg: HttpLlmReasonerConfig) -> None:
        self._cfg = cfg

    async def infer(self, data: ReasonerInput) -> Optional[ReasonerResult]:
        try:
            payload = self._build_payload(data)
            raw_text = await asyncio.to_thread(self._post_and_read, payload)
            obj = self._parse_json(raw_text)
            if obj is None:
                return None
            return sanitize_result(obj, provider=self._cfg.provider)
        except Exception:
            return None

    def _build_payload(self, data: ReasonerInput) -> Dict[str, object]:
        user = {
            "text": data.text,
            "hint": {
                "intent": data.intent_hint,
                "confidence": data.confidence_hint,
                "form": data.form_hint,
                "polarity": data.polarity_hint,
                "top_intents": data.top_intents,
                "top_scores": data.top_scores,
                "margin": data.margin,
                "hits": data.hits[:8],
            },
        }
        return {
            "model": self._cfg.model,
            "messages": [
                {"role": "system", "content": _SYSTEM_PROMPT},
                {"role": "user", "content": json.dumps(user, ensure_ascii=False)},
            ],
            "temperature": float(self._cfg.temperature),
            "max_tokens": int(self._cfg.max_output_tokens),
            "response_format": {"type": "json_object"},
        }

    def _post_and_read(self, payload: Dict[str, object]) -> str:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            self._cfg.endpoint,
            data=body,
            headers=self._headers(),
            method="POST",
        )
        timeout_s = max(0.2, float(self._cfg.timeout_ms) / 1000.0)
        try:
            with urllib.request.urlopen(req, timeout=timeout_s) as resp:
                raw = resp.read().decode("utf-8", errors="ignore")
                data = json.loads(raw)
                return self._extract_content(data)
        except urllib.error.HTTPError as exc:
            _ = exc.read()
            raise

    def _headers(self) -> Dict[str, str]:
        headers = {"Content-Type": "application/json"}
        if self._cfg.api_key:
            headers["Authorization"] = f"Bearer {self._cfg.api_key}"
        return headers

    @staticmethod
    def _extract_content(data: Dict[str, object]) -> str:
        choices = data.get("choices")
        if not isinstance(choices, list) or not choices:
            return ""
        first = choices[0]
        if not isinstance(first, dict):
            return ""
        message = first.get("message")
        if not isinstance(message, dict):
            return ""
        content = message.get("content")
        if isinstance(content, str):
            return content
        if isinstance(content, list):
            parts = []
            for item in content:
                if isinstance(item, dict) and item.get("type") in ("text", "output_text"):
                    txt = item.get("text")
                    if isinstance(txt, str):
                        parts.append(txt)
            return "\n".join(parts)
        return ""

    @staticmethod
    def _parse_json(text: str) -> Optional[Dict[str, object]]:
        if not text:
            return None
        text = text.strip()
        try:
            obj = json.loads(text)
            if isinstance(obj, dict):
                return obj
        except Exception:
            pass
        m = _JSON_RE.search(text)
        if not m:
            return None
        try:
            obj = json.loads(m.group(0))
            if isinstance(obj, dict):
                return obj
        except Exception:
            return None
        return None

