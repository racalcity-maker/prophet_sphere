from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

DOMAIN_INTENTS: Tuple[str, ...] = (
    "unknown",
    "love",
    "future",
    "choice",
    "money",
    "path",
    "danger",
    "inner_state",
    "wish",
    "past",
    "time",
    "place",
    "uncertain",
    "joke",
    "forbidden",
)

FORMS: Tuple[str, ...] = ("open", "yes_no")
POLARITIES: Tuple[str, ...] = ("positive", "negative", "neutral")


@dataclass
class ReasonerInput:
    text: str
    intent_hint: str = "unknown"
    confidence_hint: float = 0.0
    form_hint: str = "open"
    polarity_hint: str = "neutral"
    top_intents: List[str] = field(default_factory=list)
    top_scores: List[float] = field(default_factory=list)
    margin: float = 0.0
    hits: List[str] = field(default_factory=list)


@dataclass
class ReasonerResult:
    intent: str
    form: str
    polarity: str
    confidence: float
    provider: str
    raw: Dict[str, object] = field(default_factory=dict)


def sanitize_result(raw: Dict[str, object], provider: str) -> Optional[ReasonerResult]:
    if not isinstance(raw, dict):
        return None

    intent = str(raw.get("intent", "")).strip().lower().replace("timing", "time")
    if intent == "yes_no":
        # yes_no is form, not domain.
        intent = "unknown"
    if intent not in DOMAIN_INTENTS:
        intent = "unknown"

    form = str(raw.get("form", "")).strip().lower()
    if form not in FORMS:
        form = "open"

    polarity = str(raw.get("polarity", "")).strip().lower()
    if polarity not in POLARITIES:
        polarity = "neutral"

    try:
        conf = float(raw.get("confidence", 0.0))
    except Exception:
        conf = 0.0
    if conf < 0.0:
        conf = 0.0
    if conf > 0.999:
        conf = 0.999

    return ReasonerResult(
        intent=intent,
        form=form,
        polarity=polarity,
        confidence=conf,
        provider=provider,
        raw=raw,
    )

