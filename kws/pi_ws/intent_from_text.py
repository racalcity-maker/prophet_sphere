from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Tuple

VALID_INTENTS = (
    "unknown",
    "love",
    "future",
    "choice",
    "money",
    "path",
    "danger",
    "inner_state",
    "wish",
    "yes_no",
    "past",
    "time",
    "place",
    "uncertain",
    "joke",
    "forbidden",
)

_INTENT_ALIASES = {
    "where": "place",
    "yesno": "yes_no",
    "innerstate": "inner_state",
    "timing": "time",
}

_TOKEN_RE = re.compile(r"[a-z\u0430-\u044f0-9_]+", flags=re.IGNORECASE)
_PATTERN_TOKEN_RE = re.compile(r"[^a-z\u0430-\u044f0-9_*]+", flags=re.IGNORECASE)
_DEFAULT_PRIORITY = [
    "choice",
    "danger",
    "love",
    "money",
    "inner_state",
    "wish",
    "path",
    "time",
    "future",
    "yes_no",
]
_NON_DOMAIN_INTENTS = {"unknown", "yes_no", "uncertain"}
_DANGER_HINT_STEMS = ("гроз", "угроз", "угрож", "опас")
_JOKE_HINT_STEMS = (
    "шутк",
    "пошут",
    "анекдот",
    "прикол",
    "прикалыв",
    "юмор",
    "смешн",
    "рассмеш",
)
_FORBIDDEN_HINT_STEMS = (
    "хуй",
    "хуе",
    "хуй",
    "хуйн",
    "нахуй",
    "пизд",
    "пиздец",
    "еб",
    "ебан",
    "ебуч",
    "уеб",
    "долбоеб",
    "бля",
    "бляд",
    "сук",
    "мраз",
    "гандон",
    # mild insults / toxic wording (non-obscene)
    "долбан",
    "дурац",
    "туп",
    "идиот",
    "дебил",
    "кретин",
    "придур",
    "урод",
    "убог",
    "отстой",
    "твар",
    "ничтож",
    "мерзк",
    "паршив",
    "помойн",
)
_INFO_STOPWORDS = {
    "и",
    "в",
    "во",
    "на",
    "к",
    "ко",
    "с",
    "со",
    "о",
    "об",
    "обо",
    "по",
    "про",
    "у",
    "от",
    "до",
    "за",
    "из",
    "я",
    "ты",
    "он",
    "она",
    "мы",
    "вы",
    "они",
    "мне",
    "меня",
    "мой",
    "моя",
    "мое",
    "мои",
    "твой",
    "твоя",
    "его",
    "ее",
    "их",
    "ли",
    "же",
    "бы",
    "это",
    "этот",
    "эта",
    "эти",
    "то",
    "там",
    "тут",
    "что",
    "как",
    "когда",
}


@dataclass
class IntentRule:
    exact_phrases: List[Tuple[str, float]] = field(default_factory=list)
    phrase_patterns: List[Tuple[str, re.Pattern[str], float]] = field(default_factory=list)
    keywords: Dict[str, float] = field(default_factory=dict)
    negative_keywords: Dict[str, float] = field(default_factory=dict)
    min_score: float = 42.0
    min_margin: float = 12.0


@dataclass
class IntentRuleset:
    rules: Dict[str, IntentRule]
    priority: List[str]
    consensus_bonus: float
    unknown_low_score: float
    unknown_min_informative_tokens: int


def _normalize_text(text: str) -> str:
    text = (text or "").lower().replace("\u0451", "\u0435")
    return " ".join(_TOKEN_RE.findall(text))


def _normalize_token(token: str) -> str:
    return _normalize_text(token).strip()


def _normalize_pattern(text: str) -> str:
    text = (text or "").lower().replace("\u0451", "\u0435")
    text = _PATTERN_TOKEN_RE.sub(" ", text)
    return " ".join(text.split())


def tokenize_ru(text: str) -> List[str]:
    return _normalize_text(text).split()


def _contains_phrase(text_norm: str, phrase_norm: str) -> bool:
    if not phrase_norm:
        return False
    return f" {phrase_norm} " in f" {text_norm} "


def _compile_pattern(pattern_norm: str) -> re.Pattern[str]:
    escaped = re.escape(pattern_norm)
    escaped = escaped.replace(r"\*", r".*")
    escaped = escaped.replace(r"\ ", r"\s+")
    return re.compile(rf"(?:^|\s){escaped}(?:\s|$)", flags=re.IGNORECASE)


def _parse_weighted_keywords(value: Any, default_weight: float) -> Dict[str, float]:
    out: Dict[str, float] = {}
    if isinstance(value, dict):
        for k, w in value.items():
            if not isinstance(k, str):
                continue
            nk = _normalize_token(k)
            if not nk:
                continue
            try:
                out[nk] = float(w)
            except Exception:
                continue
        return out

    if isinstance(value, list):
        for item in value:
            if isinstance(item, str):
                nk = _normalize_token(item)
                if nk:
                    out[nk] = default_weight
            elif isinstance(item, dict):
                text = item.get("text") or item.get("token") or item.get("stem") or item.get("word")
                if not isinstance(text, str):
                    continue
                nk = _normalize_token(text)
                if not nk:
                    continue
                try:
                    out[nk] = float(item.get("weight", default_weight))
                except Exception:
                    out[nk] = default_weight
    return out


def _parse_weighted_phrases(value: Any, default_weight: float, phrase_key: str) -> List[Tuple[str, float]]:
    out: List[Tuple[str, float]] = []
    if isinstance(value, list):
        for item in value:
            if isinstance(item, str):
                phrase = _normalize_pattern(item) if phrase_key == "pattern" else _normalize_text(item)
                if phrase:
                    out.append((phrase, default_weight))
            elif isinstance(item, dict):
                raw = item.get(phrase_key) or item.get("text")
                if not isinstance(raw, str):
                    continue
                phrase = _normalize_pattern(raw) if phrase_key == "pattern" else _normalize_text(raw)
                if not phrase:
                    continue
                try:
                    w = float(item.get("weight", default_weight))
                except Exception:
                    w = default_weight
                out.append((phrase, w))
    elif isinstance(value, dict):
        for phrase, weight in value.items():
            if not isinstance(phrase, str):
                continue
            p = _normalize_pattern(phrase) if phrase_key == "pattern" else _normalize_text(phrase)
            if not p:
                continue
            try:
                w = float(weight)
            except Exception:
                w = default_weight
            out.append((p, w))
    return out


def _parse_ruleset(raw: Dict[str, Any]) -> IntentRuleset:
    settings = raw.get("_settings", {}) if isinstance(raw, dict) else {}
    intents_raw = raw.get("intents", raw) if isinstance(raw, dict) else {}
    if not isinstance(settings, dict):
        settings = {}
    if not isinstance(intents_raw, dict):
        intents_raw = {}

    canonical_intents: Dict[str, Any] = {}
    for key, value in intents_raw.items():
        if not isinstance(key, str):
            continue
        norm_key = _normalize_token(key)
        canon_key = _INTENT_ALIASES.get(norm_key, norm_key)
        if canon_key not in VALID_INTENTS or canon_key == "unknown":
            continue
        if canon_key not in canonical_intents:
            canonical_intents[canon_key] = value
            continue
        prev = canonical_intents[canon_key]
        if isinstance(prev, dict) and isinstance(value, dict):
            merged = dict(prev)
            merged.update(value)
            canonical_intents[canon_key] = merged
        elif isinstance(value, dict):
            canonical_intents[canon_key] = value
    intents_raw = canonical_intents

    default_min_score = float(settings.get("default_min_score", 42.0))
    default_min_margin = float(settings.get("default_min_margin", 12.0))
    consensus_bonus = float(settings.get("consensus_bonus", 15.0))

    raw_priority = settings.get("priority", _DEFAULT_PRIORITY)
    if not isinstance(raw_priority, list):
        raw_priority = list(_DEFAULT_PRIORITY)
    priority: List[str] = []
    for item in raw_priority:
        if not isinstance(item, str):
            continue
        norm_item = _normalize_token(item)
        canon_item = _INTENT_ALIASES.get(norm_item, norm_item)
        if canon_item in VALID_INTENTS and canon_item != "unknown" and canon_item not in priority:
            priority.append(canon_item)
    if not priority:
        priority = list(_DEFAULT_PRIORITY)

    rules: Dict[str, IntentRule] = {}
    for intent in VALID_INTENTS:
        if intent == "unknown":
            continue
        cfg = intents_raw.get(intent, {})
        rule = IntentRule(min_score=default_min_score, min_margin=default_min_margin)

        if isinstance(cfg, dict):
            rule.exact_phrases = _parse_weighted_phrases(cfg.get("exact_phrases", []), 100.0, "text")
            raw_patterns = _parse_weighted_phrases(cfg.get("phrase_patterns", []), 55.0, "pattern")
            rule.phrase_patterns = [(p, _compile_pattern(p), w) for p, w in raw_patterns]
            rule.keywords = _parse_weighted_keywords(cfg.get("keywords", {}), 12.0)
            rule.negative_keywords = _parse_weighted_keywords(cfg.get("negative_keywords", {}), 20.0)
            try:
                rule.min_score = float(cfg.get("min_score", default_min_score))
            except Exception:
                rule.min_score = default_min_score
            try:
                rule.min_margin = float(cfg.get("min_margin", default_min_margin))
            except Exception:
                rule.min_margin = default_min_margin
        rules[intent] = rule

    return IntentRuleset(
        rules=rules,
        priority=priority,
        consensus_bonus=consensus_bonus,
        unknown_low_score=float(settings.get("unknown_low_score", 14.0)),
        unknown_min_informative_tokens=int(settings.get("unknown_min_informative_tokens", 2)),
    )


def load_intent_keywords(path: Path) -> IntentRuleset:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        return _parse_ruleset({})
    return _parse_ruleset(raw)


def _intent_rank_key(intent: str, score: float, ruleset: IntentRuleset) -> Tuple[float, int]:
    try:
        prio = ruleset.priority.index(intent)
    except ValueError:
        prio = 999
    return (score, -prio)


def _is_informative_token(token: str) -> bool:
    if len(token) < 3:
        return False
    return token not in _INFO_STOPWORDS


def _calc_informativeness(tokens: List[str]) -> Dict[str, int]:
    informative = [t for t in tokens if _is_informative_token(t)]
    return {
        "token_count": len(tokens),
        "informative_count": len(informative),
        "informative_unique": len(set(informative)),
    }


def infer_intent_from_text(text: str, ruleset: IntentRuleset) -> Tuple[str, float, Dict[str, Any]]:
    tokens = tokenize_ru(text)
    text_norm = " ".join(tokens)
    if not tokens:
        return "unknown", 0.0, {
            "scores": {},
            "top": [],
            "top_scores": [],
            "margin": 0.0,
            "hits": {},
            "stage": "intent_v2",
            "reason": "empty_tokens",
        }

    # Service-intent shortcut: profanity/abuse should map to forbidden.
    forbidden_hits = [tok for tok in tokens if any(tok.startswith(stem) for stem in _FORBIDDEN_HINT_STEMS)]
    if forbidden_hits:
        uniq = sorted(set(forbidden_hits))
        return "forbidden", 0.99, {
            "scores": {"forbidden": 100.0},
            "top": ["forbidden", "unknown"],
            "top_scores": [100.0, 0.0],
            "margin": 100.0,
            "hits": {"forbidden": [f"heur:forbidden_hint:{','.join(uniq)}"]},
            "stage": "intent_v2",
            "reason": "forbidden_hint",
        }

    # Service-intent shortcut: explicit joke request should not compete with domains.
    joke_hits = [tok for tok in tokens if any(tok.startswith(stem) for stem in _JOKE_HINT_STEMS)]
    if joke_hits:
        uniq = sorted(set(joke_hits))
        return "joke", 0.99, {
            "scores": {"joke": 100.0},
            "top": ["joke", "unknown"],
            "top_scores": [100.0, 0.0],
            "margin": 100.0,
            "hits": {"joke": [f"heur:joke_hint:{','.join(uniq)}"]},
            "stage": "intent_v2",
            "reason": "joke_hint",
        }

    scores: Dict[str, float] = {i: 0.0 for i in ruleset.rules.keys()}
    hits: Dict[str, List[str]] = {i: [] for i in ruleset.rules.keys()}
    positive_hits: Dict[str, int] = {i: 0 for i in ruleset.rules.keys()}

    for intent, rule in ruleset.rules.items():
        for phrase, weight in rule.exact_phrases:
            if _contains_phrase(text_norm, phrase):
                scores[intent] += weight
                positive_hits[intent] += 1
                hits[intent].append(f"phrase:{phrase}(+{weight:g})")

        for raw_pat, pat_re, weight in rule.phrase_patterns:
            if pat_re.search(text_norm):
                scores[intent] += weight
                positive_hits[intent] += 1
                hits[intent].append(f"pattern:{raw_pat}(+{weight:g})")

        for stem, weight in rule.keywords.items():
            count = 0
            for token in tokens:
                if token.startswith(stem):
                    count += 1
            if count > 0:
                add = weight * float(min(count, 2))
                scores[intent] += add
                positive_hits[intent] += min(count, 2)
                hits[intent].append(f"kw:{stem}x{count}(+{add:g})")

        for stem, weight in rule.negative_keywords.items():
            count = 0
            for token in tokens:
                if token.startswith(stem):
                    count += 1
            if count > 0:
                sub = weight * float(min(count, 2))
                scores[intent] -= sub
                hits[intent].append(f"neg:{stem}x{count}(-{sub:g})")

        if positive_hits[intent] >= 2:
            scores[intent] += ruleset.consensus_bonus
            hits[intent].append(f"consensus(+{ruleset.consensus_bonus:g})")

    # Domain rescue: "грозит ли..." often appears without explicit "опасность/угроза".
    # Give danger a small deterministic boost when only danger stems are present.
    if "danger" in scores and scores.get("danger", 0.0) <= 0.0:
        danger_hint_count = 0
        for token in tokens:
            if any(token.startswith(stem) for stem in _DANGER_HINT_STEMS):
                danger_hint_count += 1
        if danger_hint_count > 0:
            add = 14.0 * float(min(2, danger_hint_count))
            scores["danger"] += add
            positive_hits["danger"] += min(2, danger_hint_count)
            hits["danger"].append(f"heur:danger_hintx{danger_hint_count}(+{add:g})")

    # Stage A: domain intent only.
    ranked_domain = sorted(
        ((intent, score) for intent, score in scores.items() if intent not in _NON_DOMAIN_INTENTS),
        key=lambda kv: _intent_rank_key(kv[0], kv[1], ruleset),
        reverse=True,
    )
    if not ranked_domain:
        return "unknown", 0.0, {
            "scores": scores,
            "top": [],
            "top_scores": [],
            "margin": 0.0,
            "hits": hits,
            "stage": "intent_v2",
            "reason": "no_domain_rules",
        }

    best_intent, best_score = ranked_domain[0]
    second_intent, second_score = ranked_domain[1] if len(ranked_domain) > 1 else ("unknown", 0.0)
    margin = best_score - second_score

    info = _calc_informativeness(tokens)
    low_info = info["informative_count"] < max(1, int(ruleset.unknown_min_informative_tokens))
    low_score = best_score < float(ruleset.unknown_low_score)

    if low_score and low_info:
        return "unknown", 0.0, {
            "scores": scores,
            "top": [best_intent, second_intent],
            "top_scores": [best_score, second_score],
            "margin": margin,
            "hits": hits,
            "stage": "intent_v2",
            "reason": "low_score_and_low_info",
            "domain": {
                "selected": best_intent,
                "score": best_score,
                "second": second_intent,
                "second_score": second_score,
                "margin": margin,
                "low_score": low_score,
                "low_info": low_info,
                "informative_count": info["informative_count"],
                "informative_unique": info["informative_unique"],
            },
        }

    positive_sum = sum(max(0.0, s) for _, s in ranked_domain)
    if positive_sum <= 0.0 or best_score <= 0.0:
        return "unknown", 0.0, {
            "scores": scores,
            "top": [best_intent, second_intent],
            "top_scores": [best_score, second_score],
            "margin": margin,
            "hits": hits,
            "stage": "intent_v2",
            "reason": "no_positive_domain_signal",
            "domain": {
                "selected": best_intent,
                "score": best_score,
                "second": second_intent,
                "second_score": second_score,
                "margin": margin,
                "low_score": low_score,
                "low_info": low_info,
                "informative_count": info["informative_count"],
                "informative_unique": info["informative_unique"],
            },
        }

    ratio = best_score / positive_sum
    margin_bonus = min(0.20, max(0.0, margin) / 120.0)
    conf = min(0.99, max(0.01, ratio + margin_bonus))

    best_intent = _INTENT_ALIASES.get(best_intent, best_intent)
    second_intent = _INTENT_ALIASES.get(second_intent, second_intent)

    # Stage B (question form/polarity) is handled by text_bank in server.py.
    return best_intent, conf, {
        "scores": scores,
        "top": [best_intent, second_intent],
        "top_scores": [best_score, second_score],
        "margin": margin,
        "hits": hits,
        "stage": "intent_v2",
        "domain": {
            "selected": best_intent,
            "score": best_score,
            "second": second_intent,
            "second_score": second_score,
            "margin": margin,
            "low_score": low_score,
            "low_info": low_info,
            "informative_count": info["informative_count"],
            "informative_unique": info["informative_unique"],
        },
        "form": {
            "yes_no_score": float(scores.get("yes_no", 0.0)),
            "yes_no_hits": len(hits.get("yes_no", [])),
        },
    }
