from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, Optional


@dataclass
class OracleLlmInput:
    text: str
    intent: str
    question_form: str
    polarity: str
    confidence: float = 0.0


@dataclass
class OracleLlmScript:
    greeting: str
    understanding: str
    prediction: str
    farewell: str
    provider: str
    raw: Dict[str, object] = field(default_factory=dict)

    @property
    def text(self) -> str:
        lines = [self.greeting, self.understanding, self.prediction, self.farewell]
        clean = [line.strip() for line in lines if line and line.strip()]
        return "\n".join(clean).strip()


def _norm_line(value: object) -> str:
    if not isinstance(value, str):
        return ""
    return " ".join(value.strip().split())


def sanitize_script(raw: Dict[str, object], *, provider: str) -> Optional[OracleLlmScript]:
    if not isinstance(raw, dict):
        return None

    phases_obj = raw.get("phases")
    phases = phases_obj if isinstance(phases_obj, dict) else {}

    def _get(*keys: str) -> str:
        for key in keys:
            if key in raw:
                value = _norm_line(raw.get(key))
                if value:
                    return value
            if key in phases:
                value = _norm_line(phases.get(key))
                if value:
                    return value
        return ""

    greeting = _get("greeting", "intro", "приветствие")
    understanding = _get("understanding", "insight", "понимание")
    prediction = _get("prediction", "answer", "предсказание")
    farewell = _get("farewell", "outro", "прощание")

    if not (greeting and understanding and prediction and farewell):
        return None

    script = OracleLlmScript(
        greeting=greeting,
        understanding=understanding,
        prediction=prediction,
        farewell=farewell,
        provider=provider,
        raw=raw,
    )
    if not script.text:
        return None
    return script
