from __future__ import annotations

import json
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

_DOMAIN_ALIASES = {
    "time": "timing",
    "timing": "timing",
    "path": "future",
    "past": "future",
    "luck": "future",
    "yes_no": "future",
    "warning": "danger",
    "self": "inner_state",
}


def _norm_token(value: str) -> str:
    return value.strip().lower().replace("-", "_").replace(" ", "_")


def _join_phrase(parts: List[str]) -> str:
    text = " ".join(part.strip() for part in parts if part.strip())
    return re.sub(r"\s+", " ", text).strip()


def _extend_unique(dst: List[str], src: Iterable[str]) -> None:
    seen = set(dst)
    for item in src:
        if not item:
            continue
        if item in seen:
            continue
        dst.append(item)
        seen.add(item)


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


def _phrase_similarity_ratio(a: str, b: str) -> float:
    toks_a = set(_tokenize_norm(a))
    toks_b = set(_tokenize_norm(b))
    if not toks_a or not toks_b:
        return 0.0
    inter = len(toks_a & toks_b)
    if inter <= 0:
        return 0.0
    union = len(toks_a | toks_b)
    if union <= 0:
        return 0.0
    return inter / union


def _phrases_redundant(a: str, b: str) -> bool:
    ta = _tokenize_norm(a)
    tb = _tokenize_norm(b)
    if not ta or not tb:
        return False
    set_a = set(ta)
    set_b = set(tb)
    inter = len(set_a & set_b)
    if inter <= 0:
        return False
    cov_a = inter / len(set_a)
    cov_b = inter / len(set_b)
    if cov_a >= 0.8 or cov_b >= 0.8:
        return True
    return _phrase_similarity_ratio(a, b) >= 0.62


@dataclass
class DomainTextSet:
    greeting: List[str] = field(default_factory=list)
    understanding: List[str] = field(default_factory=list)
    prediction_positive: List[str] = field(default_factory=list)
    prediction_negative: List[str] = field(default_factory=list)
    prediction_neutral: List[str] = field(default_factory=list)
    farewell: List[str] = field(default_factory=list)


@dataclass
class SubintentRule:
    id: str
    aliases: List[str] = field(default_factory=list)
    keywords: List[str] = field(default_factory=list)
    phrases: List[str] = field(default_factory=list)
    enabled: bool = True


@dataclass
class ScriptSelection:
    text: str
    domain: str
    subintent: str
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
        subintent_rules: Optional[Dict[str, List[SubintentRule]]] = None,
        subintent_sets: Optional[Dict[str, Dict[str, DomainTextSet]]] = None,
        seed: Optional[int] = None,
    ) -> None:
        self.domains = domains
        self.services = services
        self.subintent_rules = subintent_rules or {}
        self.subintent_sets = subintent_sets or {}
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

    def _pick_distinct_phrase(
        self,
        *,
        key: str,
        values: Iterable[str],
        avoid_phrase: str,
        fallback: str = "",
    ) -> str:
        source = [v for v in values if v]
        if not source:
            return fallback
        for candidate in self._pick_many(f"{key}:distinct", source, len(source)):
            if not _phrases_redundant(avoid_phrase, candidate):
                return candidate
        return fallback

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
    def load_from_dir(
        cls,
        root: Path,
        *,
        seed: Optional[int] = None,
        format_mode: str = "auto",
        legacy_compat: bool = False,
    ) -> "OracleTextBank":
        mode = _norm_token(format_mode)
        if mode not in ("auto", "legacy", "v2"):
            mode = "auto"

        if mode == "legacy":
            return cls._load_legacy_dir(root, seed=seed)

        if mode == "v2":
            bank = cls._load_structured_dir(root, seed=seed)
            if legacy_compat:
                legacy = cls._load_legacy_dir(root, seed=seed)
                bank._merge_inplace(legacy)
            return bank

        # auto
        if cls._has_v2_layout(root):
            bank = cls._load_structured_dir(root, seed=seed)
            if legacy_compat:
                legacy = cls._load_legacy_dir(root, seed=seed)
                bank._merge_inplace(legacy)
            return bank
        return cls._load_legacy_dir(root, seed=seed)

    @staticmethod
    def _clone_domain_set(src: DomainTextSet) -> DomainTextSet:
        return DomainTextSet(
            greeting=list(src.greeting),
            understanding=list(src.understanding),
            prediction_positive=list(src.prediction_positive),
            prediction_negative=list(src.prediction_negative),
            prediction_neutral=list(src.prediction_neutral),
            farewell=list(src.farewell),
        )

    @staticmethod
    def _clone_subintent_rule(src: SubintentRule) -> SubintentRule:
        return SubintentRule(
            id=src.id,
            aliases=list(src.aliases),
            keywords=list(src.keywords),
            phrases=list(src.phrases),
            enabled=bool(src.enabled),
        )

    def _merge_inplace(self, other: "OracleTextBank") -> None:
        for name, incoming in other.domains.items():
            current = self.domains.get(name)
            if current is None:
                self.domains[name] = self._clone_domain_set(incoming)
                continue
            _extend_unique(current.greeting, incoming.greeting)
            _extend_unique(current.understanding, incoming.understanding)
            _extend_unique(current.prediction_positive, incoming.prediction_positive)
            _extend_unique(current.prediction_negative, incoming.prediction_negative)
            _extend_unique(current.prediction_neutral, incoming.prediction_neutral)
            _extend_unique(current.farewell, incoming.farewell)

        for section, incoming_items in other.services.items():
            bucket = self.services.setdefault(section, [])
            _extend_unique(bucket, incoming_items)

        for domain, incoming_rules in other.subintent_rules.items():
            bucket = self.subintent_rules.setdefault(domain, [])
            existing_ids = {r.id for r in bucket}
            for rule in incoming_rules:
                if rule.id in existing_ids:
                    continue
                bucket.append(self._clone_subintent_rule(rule))
                existing_ids.add(rule.id)

        for domain, incoming_sets in other.subintent_sets.items():
            target_sets = self.subintent_sets.setdefault(domain, {})
            for sub_id, incoming_set in incoming_sets.items():
                current = target_sets.get(sub_id)
                if current is None:
                    target_sets[sub_id] = self._clone_domain_set(incoming_set)
                    continue
                _extend_unique(current.greeting, incoming_set.greeting)
                _extend_unique(current.understanding, incoming_set.understanding)
                _extend_unique(current.prediction_positive, incoming_set.prediction_positive)
                _extend_unique(current.prediction_negative, incoming_set.prediction_negative)
                _extend_unique(current.prediction_neutral, incoming_set.prediction_neutral)
                _extend_unique(current.farewell, incoming_set.farewell)

    @classmethod
    def _load_legacy_dir(cls, root: Path, *, seed: Optional[int] = None) -> "OracleTextBank":
        domains: Dict[str, DomainTextSet] = {}
        services: Dict[str, List[str]] = {
            "intro": [],
            "retry": [],
            "fail": [],
            "joke": [],
            "forbidden": [],
            "thinking": [],
            "resolved": [],
            "restored": [],
        }
        if not root.exists():
            return cls(domains=domains, services=services, seed=seed)

        for txt_path in sorted(root.glob("*.txt")):
            name = _norm_token(txt_path.stem)
            raw = txt_path.read_text(encoding="utf-8", errors="ignore")
            if "service_audio_txt" in name:
                parsed = cls._parse_service_text(raw)
                for k, v in parsed.items():
                    _extend_unique(services[k], v)
            else:
                parsed = cls._parse_domain_text(raw)
                current = domains.get(name)
                if current is None:
                    domains[name] = parsed
                else:
                    _extend_unique(current.greeting, parsed.greeting)
                    _extend_unique(current.understanding, parsed.understanding)
                    _extend_unique(current.prediction_positive, parsed.prediction_positive)
                    _extend_unique(current.prediction_negative, parsed.prediction_negative)
                    _extend_unique(current.prediction_neutral, parsed.prediction_neutral)
                    _extend_unique(current.farewell, parsed.farewell)

        return cls(domains=domains, services=services, seed=seed)

    @staticmethod
    def _has_v2_layout(root: Path) -> bool:
        if not root.exists():
            return False
        for name in ("activation", "service", "bridge", "answers", "closure"):
            if (root / name).is_dir():
                return True
        return False

    @staticmethod
    def _parse_json_phrases(payload: object) -> List[str]:
        out: List[str] = []
        if isinstance(payload, str):
            phrase = _join_phrase([payload])
            if phrase:
                out.append(phrase)
            return out
        if isinstance(payload, list):
            for item in payload:
                out.extend(OracleTextBank._parse_json_phrases(item))
            return out
        if isinstance(payload, dict):
            for value in payload.values():
                out.extend(OracleTextBank._parse_json_phrases(value))
            return out
        return out

    @classmethod
    def _load_structured_dir(cls, root: Path, *, seed: Optional[int] = None) -> "OracleTextBank":
        domains: Dict[str, DomainTextSet] = {}
        services: Dict[str, List[str]] = {
            "intro": [],
            "retry": [],
            "fail": [],
            "joke": [],
            "forbidden": [],
            "thinking": [],
            "resolved": [],
            "restored": [],
        }
        subintent_rules: Dict[str, List[SubintentRule]] = {}
        subintent_sets: Dict[str, Dict[str, DomainTextSet]] = {}
        if not root.exists():
            return cls(
                domains=domains,
                services=services,
                subintent_rules=subintent_rules,
                subintent_sets=subintent_sets,
                seed=seed,
            )

        activation_dir = root / "activation"
        service_dir = root / "service"
        bridge_dir = root / "bridge"
        closure_dir = root / "closure"
        answers_dir = root / "answers"
        dictionaries_dir = root / "dictionaries"

        global_greeting: List[str] = []
        global_understanding: List[str] = []
        global_farewell: List[str] = []

        def _load_json(path: Path) -> object:
            if not path.exists():
                return {}
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except Exception:
                return {}

        def _load_json_list(path: Path) -> List[str]:
            return cls._parse_json_phrases(_load_json(path))

        def _load_json_map(path: Path) -> Dict[str, List[str]]:
            payload = _load_json(path)
            out: Dict[str, List[str]] = {}
            if not isinstance(payload, dict):
                return out
            for key, value in payload.items():
                out[_norm_token(str(key))] = cls._parse_json_phrases(value)
            return out

        if activation_dir.exists():
            _extend_unique(global_greeting, _load_json_list(activation_dir / "listen.json"))
            _extend_unique(global_understanding, _load_json_list(activation_dir / "reprompt.json"))
            _extend_unique(global_understanding, _load_json_list(activation_dir / "idle_prompt.json"))

        if service_dir.exists():
            _extend_unique(services["fail"], _load_json_list(service_dir / "errors.json"))
            _extend_unique(services["fail"], _load_json_list(service_dir / "fallback.json"))
            _extend_unique(services["retry"], _load_json_list(service_dir / "busy.json"))
            status_sections = _load_json_map(service_dir / "system_status.json")
            _extend_unique(services["intro"], status_sections.get("listening", []))
            _extend_unique(services["thinking"], status_sections.get("thinking", []))
            _extend_unique(services["resolved"], status_sections.get("resolved", []))
            _extend_unique(services["restored"], status_sections.get("restored", []))
            _extend_unique(services["joke"], _load_json_list(service_dir / "joke.json"))
            _extend_unique(services["forbidden"], _load_json_list(service_dir / "insult_replies.json"))

        if bridge_dir.exists():
            for bridge_file in sorted(bridge_dir.glob("*.json")):
                _extend_unique(global_understanding, _load_json_list(bridge_file))

        if closure_dir.exists():
            for closure_file in sorted(closure_dir.glob("*.json")):
                _extend_unique(global_farewell, _load_json_list(closure_file))

        if not services["intro"]:
            _extend_unique(services["intro"], global_greeting)
        if not services["retry"]:
            _extend_unique(services["retry"], global_understanding)
        if not services["fail"]:
            _extend_unique(services["fail"], global_farewell)

        answer_bucket_positive = {
            "clear_yes",
            "lean_yes",
            "timing_soon",
            "guidance_act",
        }
        answer_bucket_negative = {
            "clear_no",
            "lean_no",
            "timing_later",
            "guidance_wait",
            "warning",
        }

        if answers_dir.exists():
            for domain_dir in sorted([p for p in answers_dir.iterdir() if p.is_dir()]):
                domain_name = _norm_token(domain_dir.name)
                domain_name = _DOMAIN_ALIASES.get(domain_name, domain_name)
                target = domains.setdefault(domain_name, DomainTextSet())
                per_domain = subintent_sets.setdefault(domain_name, {})
                for json_path in sorted(domain_dir.glob("*.json")):
                    sub_id = _norm_token(json_path.stem)
                    sub_target = per_domain.setdefault(sub_id, DomainTextSet())
                    payload = _load_json(json_path)
                    if isinstance(payload, dict):
                        for key, value in payload.items():
                            phrases = cls._parse_json_phrases(value)
                            if not phrases:
                                continue
                            key_norm = _norm_token(str(key))
                            if key_norm in answer_bucket_positive:
                                _extend_unique(target.prediction_positive, phrases)
                                _extend_unique(sub_target.prediction_positive, phrases)
                            elif key_norm in answer_bucket_negative:
                                _extend_unique(target.prediction_negative, phrases)
                                _extend_unique(sub_target.prediction_negative, phrases)
                            else:
                                _extend_unique(target.prediction_neutral, phrases)
                                _extend_unique(sub_target.prediction_neutral, phrases)
                    else:
                        phrases = cls._parse_json_phrases(payload)
                        _extend_unique(target.prediction_neutral, phrases)
                        _extend_unique(sub_target.prediction_neutral, phrases)

        subintents_path = dictionaries_dir / "subintents.json"
        if subintents_path.exists():
            raw_sub = _load_json(subintents_path)
            raw_map = raw_sub.get("subintents", {}) if isinstance(raw_sub, dict) else {}
            if isinstance(raw_map, dict):
                for domain_key, items in raw_map.items():
                    domain_name = _norm_token(str(domain_key))
                    domain_name = _DOMAIN_ALIASES.get(domain_name, domain_name)
                    if not isinstance(items, list):
                        continue
                    domain_rules = subintent_rules.setdefault(domain_name, [])
                    existing = {r.id for r in domain_rules}
                    for item in items:
                        if not isinstance(item, dict):
                            continue
                        sid = _norm_token(str(item.get("id", "")))
                        if not sid or sid in existing:
                            continue
                        phrases = cls._parse_json_phrases(item.get("phrases", []))
                        aliases = cls._parse_json_phrases(item.get("aliases", []))
                        keywords = cls._parse_json_phrases(item.get("keywords", []))
                        enabled = bool(item.get("enabled", True))
                        domain_rules.append(
                            SubintentRule(
                                id=sid,
                                aliases=aliases,
                                keywords=keywords,
                                phrases=phrases,
                                enabled=enabled,
                            )
                        )
                        existing.add(sid)

        for domain in domains.values():
            if not domain.greeting:
                _extend_unique(domain.greeting, global_greeting)
            if not domain.understanding:
                _extend_unique(domain.understanding, global_understanding)
            if not domain.farewell:
                _extend_unique(domain.farewell, global_farewell)

        for per_domain in subintent_sets.values():
            for sub_domain in per_domain.values():
                if not sub_domain.greeting:
                    _extend_unique(sub_domain.greeting, global_greeting)
                if not sub_domain.understanding:
                    _extend_unique(sub_domain.understanding, global_understanding)
                if not sub_domain.farewell:
                    _extend_unique(sub_domain.farewell, global_farewell)

        return cls(
            domains=domains,
            services=services,
            subintent_rules=subintent_rules,
            subintent_sets=subintent_sets,
            seed=seed,
        )

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
        candidates = [key]
        if key in _DOMAIN_ALIASES:
            candidates.append(_DOMAIN_ALIASES[key])
        candidates.extend(["future", "love", "choice"])

        for name in candidates:
            data = self.domains.get(name)
            if data is None:
                continue
            if data.greeting or data.understanding or data.prediction_neutral or data.farewell:
                return name, data
        return "unknown", DomainTextSet()

    def detect_subintent(self, text: str, *, intent: str) -> Tuple[str, float, Dict[str, object]]:
        domain_name, _ = self._resolve_domain(intent)
        rules = self.subintent_rules.get(domain_name, [])
        if not text or not rules:
            return "", 0.0, {"domain": domain_name, "reason": "no_rules_or_text", "scores": {}}

        tokens = _tokenize_norm(text)
        if not tokens:
            return "", 0.0, {"domain": domain_name, "reason": "empty_tokens", "scores": {}}
        text_norm = " ".join(tokens)
        padded = f" {text_norm} "

        scores: Dict[str, float] = {}
        hits: Dict[str, List[str]] = {}
        for rule in rules:
            if not rule.enabled:
                continue
            sid = _norm_token(rule.id)
            if not sid:
                continue
            score = 0.0
            rhits: List[str] = []

            for phrase in rule.phrases:
                phrase_norm = " ".join(_tokenize_norm(phrase))
                if not phrase_norm:
                    continue
                if f" {phrase_norm} " in padded:
                    score += 14.0
                    rhits.append(f"phrase:{phrase_norm}(+14)")

            for alias in rule.aliases:
                alias_norm = " ".join(_tokenize_norm(alias))
                if not alias_norm:
                    continue
                if f" {alias_norm} " in padded:
                    score += 9.0
                    rhits.append(f"alias:{alias_norm}(+9)")

            for kw in rule.keywords:
                kw_tokens = _tokenize_norm(kw)
                if not kw_tokens:
                    continue
                for kw_token in kw_tokens:
                    count = sum(1 for t in tokens if t.startswith(kw_token))
                    if count > 0:
                        add = 4.0 * float(min(2, count))
                        score += add
                        rhits.append(f"kw:{kw_token}x{count}(+{add:g})")

            if score > 0.0:
                scores[sid] = score
                hits[sid] = rhits

        if not scores:
            return "", 0.0, {"domain": domain_name, "reason": "no_signal", "scores": {}, "hits": {}}

        ranked = sorted(scores.items(), key=lambda kv: kv[1], reverse=True)
        best_id, best_score = ranked[0]
        second_score = ranked[1][1] if len(ranked) > 1 else 0.0
        margin = best_score - second_score
        if best_score < 4.0:
            return "", 0.0, {
                "domain": domain_name,
                "reason": "low_score",
                "scores": scores,
                "hits": hits,
                "margin": margin,
            }

        conf = min(0.99, max(0.05, (best_score / max(1.0, best_score + second_score)) + min(0.25, margin / 40.0)))
        return best_id, conf, {
            "domain": domain_name,
            "scores": scores,
            "hits": hits,
            "margin": margin,
            "top": [r[0] for r in ranked[:3]],
        }

    def select_script(
        self,
        *,
        intent: str,
        subintent: str = "",
        question_form: str = "open",
        force_polarity: str = "",
        include_opening_phases: bool = False,
        allow_thinking_phase: bool = True,
    ) -> ScriptSelection:
        normalized_intent = _norm_token(intent)
        if normalized_intent == "joke":
            text = self._pick_service("joke", fallback="Я пошучу и снова замолчу.")
            return ScriptSelection(
                text=text,
                domain="service_joke",
                subintent="",
                question_form="open",
                prediction_bucket="service",
                phases={"joke": text},
            )
        if normalized_intent == "forbidden":
            text = self._pick_service("forbidden", fallback="На этот вопрос я не отвечаю.")
            return ScriptSelection(
                text=text,
                domain="service_forbidden",
                subintent="",
                question_form="open",
                prediction_bucket="service",
                phases={"forbidden": text},
            )
        if normalized_intent in ("unknown", "uncertain"):
            text = self._pick_service("fail", fallback="Сегодня ответ скрыт. Приходи позже.")
            return ScriptSelection(
                text=text,
                domain="service_fail",
                subintent="",
                question_form="open",
                prediction_bucket="service",
                phases={"fail": text},
            )

        domain_name, domain = self._resolve_domain(normalized_intent)
        selected_subintent = _norm_token(subintent)
        active_domain = domain
        if selected_subintent:
            by_domain = self.subintent_sets.get(domain_name, {})
            candidate = by_domain.get(selected_subintent)
            if candidate is not None:
                active_domain = candidate
            else:
                selected_subintent = ""

        greeting = ""
        understanding = ""
        if include_opening_phases:
            greeting = self._picker.pick(f"{domain_name}:greeting", active_domain.greeting) or self._pick_service("intro")
            understanding = self._picker.pick(f"{domain_name}:understanding", active_domain.understanding) or self._pick_service("retry")
            if _phrases_redundant(greeting, understanding):
                understanding = self._pick_distinct_phrase(
                    key=f"{domain_name}:understanding",
                    values=active_domain.understanding,
                    avoid_phrase=greeting,
                    fallback="",
                )

        form = "yes_no" if _norm_token(question_form) == "yes_no" else "open"
        bucket = "neutral"
        pred_pool = active_domain.prediction_neutral

        if form == "yes_no":
            polarity = _norm_token(force_polarity)
            if polarity not in ("positive", "negative"):
                polarity = "positive" if self._picker.choose_bool() else "negative"
            if polarity == "positive" and active_domain.prediction_positive:
                pred_pool = active_domain.prediction_positive
                bucket = "positive"
            elif polarity == "negative" and active_domain.prediction_negative:
                pred_pool = active_domain.prediction_negative
                bucket = "negative"
            elif active_domain.prediction_neutral:
                pred_pool = active_domain.prediction_neutral
                bucket = "neutral"
            elif active_domain.prediction_positive or active_domain.prediction_negative:
                merged = list(active_domain.prediction_positive) + list(active_domain.prediction_negative)
                pred_pool = merged
                bucket = "mixed"

        prediction = self._picker.pick(f"{domain_name}:prediction:{bucket}", pred_pool)
        if not prediction:
            prediction = self._pick_service("fail", fallback="Сейчас знаки молчат")
            bucket = "service_fallback"

        thinking = ""
        if allow_thinking_phase:
            think_pool = self.services.get("thinking", [])
            if not think_pool:
                think_pool = self.services.get("retry", [])
            if think_pool and self._picker.choose_bool():
                candidate = self._picker.pick(f"{domain_name}:thinking", think_pool)
                if candidate and not _phrases_redundant(candidate, prediction):
                    thinking = candidate

        farewell = self._picker.pick(f"{domain_name}:farewell", active_domain.farewell)
        if not farewell:
            farewell = self._pick_service("fail", fallback="На сегодня всё")
        if _phrases_redundant(prediction, farewell) or (thinking and _phrases_redundant(thinking, farewell)):
            farewell = ""

        phases = {
            "greeting": greeting,
            "understanding": understanding,
            "thinking": thinking,
            "prediction": prediction,
            "farewell": farewell,
        }
        ordered_parts = [prediction, farewell]
        if thinking:
            ordered_parts = [thinking, prediction, farewell]
        if include_opening_phases:
            ordered_parts = [greeting, understanding] + ordered_parts
        text = "\n".join([part for part in ordered_parts if part]).strip()
        return ScriptSelection(
            text=text,
            domain=domain_name,
            subintent=selected_subintent,
            question_form=form,
            prediction_bucket=bucket,
            phases=phases,
        )

