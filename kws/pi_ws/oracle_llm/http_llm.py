from __future__ import annotations

import asyncio
import json
import os
import re
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Dict, Optional

from .contracts import OracleLlmInput, OracleLlmScript, sanitize_script

_JSON_RE = re.compile(r"\{.*\}", re.DOTALL)

_SYSTEM_PROMPT = (
    "Верни только JSON без markdown. "
    "Поля: greeting, understanding, prediction, farewell. "
    "Каждое поле: одна очень короткая фраза на русском (до 8 слов). "
    "Стиль: мистический, без лишней вежливости, без списков. "
    "Никаких других полей."
)


@dataclass
class HttpOracleLlmConfig:
    endpoint: str
    model: str
    timeout_ms: int = 2200
    temperature: float = 0.5
    max_output_tokens: int = 240
    api_key: str = ""
    provider: str = "oracle_local_llm"


class HttpOracleLlm:
    def __init__(self, cfg: HttpOracleLlmConfig) -> None:
        self._cfg = cfg

    async def generate(self, data: OracleLlmInput) -> Optional[OracleLlmScript]:
        try:
            payload = self._build_payload(data)
            raw_text = await asyncio.to_thread(self._post_and_read, payload)
            if self._debug_enabled():
                self._debug_log(f"raw len={len(raw_text)} preview={raw_text[:240]!r}")
            obj = self._parse_json(raw_text)
            if obj is None:
                if self._debug_enabled():
                    self._debug_log("parse_json: no JSON object found")
                return None
            scripted = sanitize_script(obj, provider=self._cfg.provider)
            if scripted is None and self._debug_enabled():
                self._debug_log(f"sanitize_script: missing required keys in {obj!r}")
            return scripted
        except Exception:
            if self._debug_enabled():
                self._debug_log("generate: exception path")
            return None

    def _build_payload(self, data: OracleLlmInput) -> Dict[str, object]:
        user = {
            "intent": data.intent,
            "question_form": data.question_form,
            "polarity": data.polarity,
            "confidence": float(data.confidence),
            "user_text": data.text,
            "style_hints": {
                "tone": "mystic",
                "concise": True,
                "single_sentence_per_phase": True,
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
        # Strip fenced markdown wrapper if present.
        text = re.sub(r"^\s*```(?:json)?\s*", "", text, flags=re.IGNORECASE)
        text = re.sub(r"\s*```\s*$", "", text)
        try:
            obj = json.loads(text)
            if isinstance(obj, dict):
                return obj
        except Exception:
            pass
        match = _JSON_RE.search(text)
        if not match:
            return None
        try:
            obj = json.loads(match.group(0))
            if isinstance(obj, dict):
                return obj
        except Exception:
            return None
        return None

    @staticmethod
    def _debug_enabled() -> bool:
        return os.getenv("ORB_ORACLE_LLM_DEBUG", "0").strip() not in ("", "0", "false", "False")

    @staticmethod
    def _debug_log(msg: str) -> None:
        print(f"[oracle_llm] {msg}")
