from __future__ import annotations

import random
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

_HEADER_GREETING_RE = re.compile(r"^\s*Greeting\b", re.IGNORECASE)
_HEADER_UNDERSTANDING_RE = re.compile(r"^\s*Understanding\b", re.IGNORECASE)
_HEADER_PREDICTION_RE = re.compile(r"^\s*Prediction\b", re.IGNORECASE)
_HEADER_FAREWELL_RE = re.compile(r"^\s*Farewell\b", re.IGNORECASE)

_HEADER_POS_RE = re.compile(r"^\s*Положительн", re.IGNORECASE)
_HEADER_NEG_RE = re.compile(r"^\s*Отрицательн", re.IGNORECASE)
_HEADER_NEU_RE = re.compile(r"^\s*Нейтральн", re.IGNORECASE)

_HEADER_SERVICE_RE = re.compile(r"^\s*(INTRO|RETRY|FAIL|JOKE|FORBIDDEN)\b", re.IGNORECASE)
_HEADER_DOMAIN_RE = re.compile(r"^\s*Домен\s*[:,]", re.IGNORECASE)

_YES_NO_HINT_RE = re.compile(
    r"\b(или\s+нет|да\s+или\s+нет|будет\s+ли|стоит\s+ли|нужно\s+ли|получится\s+ли|есть\s+ли|верно\s+ли|правда\s+ли|мой\s+ли|грозит\s+ли|опасно\s+ли)\b",
    re.IGNORECASE,
)
_TIMING_HINT_RE = re.compile(
    r"\b(когда|скоро|дата|срок|какого\s+числа|в\s+какой\s+день|через\s+сколько)\b",
    re.IGNORECASE,
)
_TOKEN_RE = re.compile(r"[a-zа-яё0-9_]+", flags=re.IGNORECASE)

_POLARITY_POS_STEMS = (
    "получ",
    "удач",
    "повез",
    "успех",
    "сбуд",
    "исполн",
    "прибыл",
    "налад",
    "лучш",
    "стабил",
    "встрет",
    "свадьб",
    "брак",
)
_POLARITY_NEG_STEMS = (
    "опас",
    "угроз",
    "потер",
    "долг",
    "проблем",
    "бед",
    "плох",
    "вред",
    "обман",
    "лож",
    "порч",
    "сглаз",
    "риск",
    "уйти",
    "расстав",
)


def _norm_token(value: str) -> str:
    return value.strip().lower().replace("-", "_").replace(" ", "_")


def _join_phrase(parts: List[str]) -> str:
    text = " ".join(part.strip() for part in parts if part.strip())
    return re.sub(r"\s+", " ", text).strip()


def _normalize_section_boundaries(raw: str) -> str:
    text = raw.replace("\ufeff", "")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    header_tokens = [
        r"Домен\s*[:,]",
        r"Greeting\s*\(",
        r"Understanding\s*\(",
        r"Prediction\s*\(",
        r"Положительные\s*\(",
        r"Отрицательные\s*\(",
        r"Нейтральные\s*\(",
        r"Farewell\s*\(",
        r"INTRO\s*\(",
        r"RETRY\s*\(",
        r"FAIL\s*\(",
        r"JOKE\s*\(",
        r"FORBIDDEN\s*\(",
    ]
    for token in header_tokens:
        text = re.sub(rf"\s+({token})", r"\n\1", text, flags=re.IGNORECASE)
    return text


def _tokenize_norm(text: str) -> List[str]:
    clean = (text or "").strip().lower().replace("ё", "е")
    return _TOKEN_RE.findall(clean)


def _count_stem_hits(tokens: List[str], stems: Tuple[str, ...]) -> int:
    total = 0
    for token in tokens:
        for stem in stems:
            if token.startswith(stem):
                total += 1
                break
    return total


@dataclass
class DomainTextSet:
    greeting: List[str] = field(default_factory=list)
    understanding: List[str] = field(default_factory=list)
    prediction_positive: List[str] = field(default_factory=list)
    prediction_negative: List[str] = field(default_factory=list)
    prediction_neutral: List[str] = field(default_factory=list)
    farewell: List[str] = field(default_factory=list)


@dataclass
class ScriptSelection:
    text: str
    domain: str
    question_form: str
    prediction_bucket: str
    phases: Dict[str, str]


class _CyclePicker:
    def __init__(self, seed: Optional[int] = None) -> None:
        self._rng = random.Random(seed)
        self._queues: Dict[str, List[str]] = {}

    def pick(self, key: str, values: Iterable[str]) -> str:
        source = [v for v in values if v]
        if not source:
            return ""
        queue = self._queues.get(key)
        if not queue:
            queue = list(source)
            self._rng.shuffle(queue)
            self._queues[key] = queue
        item = queue.pop()
        return item

    def choose_bool(self) -> bool:
        return bool(self._rng.getrandbits(1))


class OracleTextBank:
    def __init__(
        self,
        *,
        domains: Dict[str, DomainTextSet],
        services: Dict[str, List[str]],
        seed: Optional[int] = None,
    ) -> None:
        self.domains = domains
        self.services = services
        self._picker = _CyclePicker(seed)

    @staticmethod
    def _join_lines(lines: List[str]) -> str:
        return _join_phrase(lines)

    def _pick_many(self, key: str, values: Iterable[str], count: int) -> List[str]:
        source = [v for v in values if v]
        if not source or count <= 0:
            return []
        target = min(count, len(source))
        out: List[str] = []
        attempts = 0
        max_attempts = max(target * 4, len(source) * 2)
        while len(out) < target and attempts < max_attempts:
            attempts += 1
            item = self._picker.pick(key, source)
            if item and item not in out:
                out.append(item)
            if len(out) >= len(source):
                break
        return out

    @staticmethod
    def detect_question_form(text: str, *, yes_no_score: float = 0.0, yes_no_hits: int = 0) -> str:
        q = (text or "").strip().lower().replace("ё", "е")
        if not q:
            return "open"
        # Be conservative: a single weak "yes_no" hit should not flip open-domain questions.
        if yes_no_hits >= 2:
            return "yes_no"
        if yes_no_score >= 28.0:
            return "yes_no"
        if _YES_NO_HINT_RE.search(q):
            return "yes_no"
        return "open"

    @staticmethod
    def detect_timing_intent(text: str, *, current_intent: str = "unknown") -> bool:
        intent = _norm_token(current_intent)
        if intent not in ("unknown", "future", "time", "timing", "yes_no"):
            return False
        q = (text or "").strip().lower().replace("ё", "е")
        if not q:
            return False
        if _TIMING_HINT_RE.search(q):
            return True
        tokens = _tokenize_norm(q)
        if not tokens:
            return False
        timing_hits = _count_stem_hits(tokens, ("когда", "скоро", "срок", "дат", "недел", "месяц", "дн", "год"))
        return timing_hits >= 2

    @staticmethod
    def detect_prediction_polarity(
        text: str,
        *,
        question_form: str = "open",
    ) -> Tuple[str, float, Dict[str, float]]:
        if _norm_token(question_form) != "yes_no":
            return "neutral", 0.0, {"positive": 0.0, "negative": 0.0, "delta": 0.0}

        tokens = _tokenize_norm(text)
        if not tokens:
            return "neutral", 0.0, {"positive": 0.0, "negative": 0.0, "delta": 0.0}

        pos = float(_count_stem_hits(tokens, _POLARITY_POS_STEMS))
        neg = float(_count_stem_hits(tokens, _POLARITY_NEG_STEMS))
        delta = pos - neg
        abs_sum = max(1.0, pos + neg)
        conf = min(0.99, abs(delta) / abs_sum)

        if delta >= 1.0:
            return "positive", conf, {"positive": pos, "negative": neg, "delta": delta}
        if delta <= -1.0:
            return "negative", conf, {"positive": pos, "negative": neg, "delta": delta}
        return "neutral", 0.0, {"positive": pos, "negative": neg, "delta": delta}

    @classmethod
    def load_from_dir(cls, root: Path, *, seed: Optional[int] = None) -> "OracleTextBank":
        domains: Dict[str, DomainTextSet] = {}
        services: Dict[str, List[str]] = {
            "intro": [],
            "retry": [],
            "fail": [],
            "joke": [],
            "forbidden": [],
        }
        if not root.exists():
            return cls(domains=domains, services=services, seed=seed)

        for txt_path in sorted(root.glob("*.txt")):
            name = _norm_token(txt_path.stem)
            raw = txt_path.read_text(encoding="utf-8", errors="ignore")
            if "service_audio_txt" in name:
                parsed = cls._parse_service_text(raw)
                for k, v in parsed.items():
                    services[k].extend(v)
            else:
                parsed = cls._parse_domain_text(raw)
                domains[name] = parsed

        return cls(domains=domains, services=services, seed=seed)

    @staticmethod
    def _parse_service_text(raw: str) -> Dict[str, List[str]]:
        raw = _normalize_section_boundaries(raw)
        buckets: Dict[str, List[str]] = {
            "intro": [],
            "retry": [],
            "fail": [],
            "joke": [],
            "forbidden": [],
        }
        current: Optional[str] = None

        for line in raw.splitlines():
            m = _HEADER_SERVICE_RE.match(line)
            if m:
                current = m.group(1).strip().lower()
                continue
            phrase = _join_phrase([line])
            if not phrase:
                continue
            if current is None:
                continue
            buckets[current].append(phrase)
        return buckets

    @staticmethod
    def _parse_domain_text(raw: str) -> DomainTextSet:
        raw = _normalize_section_boundaries(raw)
        out = DomainTextSet()
        current: Optional[str] = None

        for line in raw.splitlines():
            if _HEADER_DOMAIN_RE.match(line):
                current = None
                continue
            if _HEADER_GREETING_RE.match(line):
                current = "greeting"
                continue
            if _HEADER_UNDERSTANDING_RE.match(line):
                current = "understanding"
                continue
            if _HEADER_PREDICTION_RE.match(line):
                current = "prediction_neutral"
                continue
            if _HEADER_POS_RE.match(line):
                current = "prediction_positive"
                continue
            if _HEADER_NEG_RE.match(line):
                current = "prediction_negative"
                continue
            if _HEADER_NEU_RE.match(line):
                current = "prediction_neutral"
                continue
            if _HEADER_FAREWELL_RE.match(line):
                current = "farewell"
                continue

            phrase = _join_phrase([line])
            if not phrase:
                continue
            if current is None:
                continue
            if current == "greeting":
                out.greeting.append(phrase)
            elif current == "understanding":
                out.understanding.append(phrase)
            elif current == "prediction_positive":
                out.prediction_positive.append(phrase)
            elif current == "prediction_negative":
                out.prediction_negative.append(phrase)
            elif current == "prediction_neutral":
                out.prediction_neutral.append(phrase)
            elif current == "farewell":
                out.farewell.append(phrase)
        return out

    def _pick_service(self, section: str, *, fallback: str = "") -> str:
        values = self.services.get(section, [])
        if values:
            return self._picker.pick(f"service:{section}", values)
        if fallback:
            return fallback
        for alt in ("fail", "retry", "intro"):
            alt_values = self.services.get(alt, [])
            if alt_values:
                return self._picker.pick(f"service:{alt}", alt_values)
        return ""

    def pick_service_phrase(self, section: str, *, fallback: str = "") -> str:
        return self._pick_service(_norm_token(section), fallback=fallback)

    def _resolve_domain(self, intent: str) -> Tuple[str, DomainTextSet]:
        key = _norm_token(intent)
        aliases = {
            "time": "timing",
            "timing": "timing",
            "path": "future",
            "past": "future",
            "luck": "future",
            "yes_no": "future",
        }
        candidates = [key]
        if key in aliases:
            candidates.append(aliases[key])
        candidates.extend(["future", "love", "choice"])

        for name in candidates:
            data = self.domains.get(name)
            if data is None:
                continue
            if data.greeting or data.understanding or data.prediction_neutral or data.farewell:
                return name, data
        return "unknown", DomainTextSet()

    def select_script(
        self,
        *,
        intent: str,
        question_form: str = "open",
        force_polarity: str = "",
    ) -> ScriptSelection:
        normalized_intent = _norm_token(intent)
        if normalized_intent == "joke":
            text = self._pick_service("joke", fallback="Я пошучу и снова замолчу.")
            return ScriptSelection(
                text=text,
                domain="service_joke",
                question_form="open",
                prediction_bucket="service",
                phases={"joke": text},
            )
        if normalized_intent == "forbidden":
            text = self._pick_service("forbidden", fallback="На этот вопрос я не отвечаю.")
            return ScriptSelection(
                text=text,
                domain="service_forbidden",
                question_form="open",
                prediction_bucket="service",
                phases={"forbidden": text},
            )
        if normalized_intent in ("unknown", "uncertain"):
            text = self._pick_service("fail", fallback="Сегодня ответ скрыт. Приходи позже.")
            return ScriptSelection(
                text=text,
                domain="service_fail",
                question_form="open",
                prediction_bucket="service",
                phases={"fail": text},
            )

        domain_name, domain = self._resolve_domain(normalized_intent)
        greeting = self._picker.pick(f"{domain_name}:greeting", domain.greeting) or self._pick_service("intro")
        understanding = self._picker.pick(f"{domain_name}:understanding", domain.understanding) or self._pick_service("retry")

        form = "yes_no" if _norm_token(question_form) == "yes_no" else "open"
        bucket = "neutral"
        pred_pool = domain.prediction_neutral

        if form == "yes_no":
            polarity = _norm_token(force_polarity)
            if polarity not in ("positive", "negative"):
                polarity = "positive" if self._picker.choose_bool() else "negative"
            if polarity == "positive" and domain.prediction_positive:
                pred_pool = domain.prediction_positive
                bucket = "positive"
            elif polarity == "negative" and domain.prediction_negative:
                pred_pool = domain.prediction_negative
                bucket = "negative"
            elif domain.prediction_neutral:
                pred_pool = domain.prediction_neutral
                bucket = "neutral"
            elif domain.prediction_positive or domain.prediction_negative:
                merged = list(domain.prediction_positive) + list(domain.prediction_negative)
                pred_pool = merged
                bucket = "mixed"

        prediction = self._picker.pick(f"{domain_name}:prediction:{bucket}", pred_pool)
        if not prediction:
            prediction = self._pick_service("fail", fallback="Сейчас знаки молчат")
            bucket = "service_fallback"

        farewell = self._picker.pick(f"{domain_name}:farewell", domain.farewell)
        if not farewell:
            farewell = self._pick_service("fail", fallback="На сегодня всё")

        phases = {
            "greeting": greeting,
            "understanding": understanding,
            "prediction": prediction,
            "farewell": farewell,
        }
        text = "\n".join([greeting, understanding, prediction, farewell]).strip()
        return ScriptSelection(
            text=text,
            domain=domain_name,
            question_form=form,
            prediction_bucket=bucket,
            phases=phases,
        )

