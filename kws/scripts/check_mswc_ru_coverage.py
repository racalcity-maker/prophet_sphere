#!/usr/bin/env python3
"""
Check Russian keyword coverage in MLCommons MSWC using split metadata only.

This script does NOT load the HF dataset script, so it works with recent
`datasets` package changes.

Usage:
  python kws/scripts/check_mswc_ru_coverage.py --map kws/archetypes_map.json
  python kws/scripts/check_mswc_ru_coverage.py --splits train,validation,test
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import sys
import tarfile
from collections import Counter
from pathlib import Path
from typing import Dict, Iterable, List, Set


def norm_token(text: str) -> str:
    token = text.strip().lower().replace("ё", "е")
    # Fix mixed Latin/Cyrillic lookalikes often present in RU MSWC metadata.
    if any("\u0400" <= ch <= "\u04ff" for ch in token):
        trans = {
            "a": "а",
            "b": "в",
            "c": "с",
            "e": "е",
            "h": "н",
            "k": "к",
            "m": "м",
            "o": "о",
            "p": "р",
            "t": "т",
            "x": "х",
            "y": "у",
        }
        token = "".join(trans.get(ch, ch) for ch in token)
    return token


def load_mapping(path: Path) -> Dict[str, List[str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("Mapping JSON must be an object: {class: [tokens...]}")

    out: Dict[str, List[str]] = {}
    for intent, words in data.items():
        if not isinstance(intent, str) or not isinstance(words, list):
            raise ValueError("Invalid mapping entry format")
        clean_words = []
        for w in words:
            if isinstance(w, str):
                token = norm_token(w)
                if token:
                    clean_words.append(token)
        if clean_words:
            out[intent] = clean_words
    return out


def build_word_to_intent(mapping: Dict[str, List[str]]) -> Dict[str, str]:
    word_to_intent: Dict[str, str] = {}
    for intent, words in mapping.items():
        for w in words:
            if w not in word_to_intent:
                word_to_intent[w] = intent
    return word_to_intent


def parse_splits(text: str) -> List[str]:
    out: List[str] = []
    for raw in text.split(","):
        s = raw.strip().lower()
        if s:
            out.append(s)
    return out


def canonical_split_name(split: str) -> str:
    s = split.strip().lower()
    if s in ("validation", "valid", "val"):
        return "dev"
    return s


def download_splits_tar(repo_id: str, lang: str, revision: str | None) -> Path:
    rel_path = f"data/splits/{lang}/splits.tar.gz"
    try:
        from huggingface_hub import hf_hub_download  # type: ignore
    except Exception as exc:
        raise RuntimeError(
            "Missing dependency: huggingface_hub. Install with: pip install huggingface_hub"
        ) from exc

    return Path(
        hf_hub_download(
            repo_id=repo_id,
            repo_type="dataset",
            filename=rel_path,
            revision=revision,
        )
    )


def iter_split_rows(tar_path: Path, wanted_splits: Set[str]) -> Iterable[dict]:
    with tarfile.open(tar_path, "r:gz") as tf:
        members = [m for m in tf.getmembers() if m.isfile() and m.name.endswith(".csv")]
        for member in members:
            split_name = canonical_split_name(Path(member.name).stem.lower())
            if split_name not in wanted_splits:
                continue

            fobj = tf.extractfile(member)
            if fobj is None:
                continue

            text = io.TextIOWrapper(fobj, encoding="utf-8", newline="")
            reader = csv.DictReader(text)
            for row in reader:
                row["_split"] = split_name
                yield row


def pick_field(row: dict, *candidates: str) -> str:
    for key in candidates:
        if key in row and row[key] is not None:
            return str(row[key])
    return ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--map", type=Path, default=Path("kws/archetypes_map.json"))
    parser.add_argument("--repo", default="MLCommons/ml_spoken_words")
    parser.add_argument("--lang", default="ru")
    parser.add_argument("--splits", default="train")
    parser.add_argument("--revision", default=None)
    parser.add_argument("--valid-only", action="store_true")
    args = parser.parse_args()

    mapping = load_mapping(args.map)
    word_to_intent = build_word_to_intent(mapping)
    target_words: Set[str] = set(word_to_intent.keys())
    wanted_splits = {canonical_split_name(s) for s in parse_splits(args.splits)}
    if not wanted_splits:
        print("ERROR: no splits selected", file=sys.stderr)
        return 2

    try:
        tar_path = download_splits_tar(args.repo, args.lang, args.revision)
    except Exception as exc:
        print(f"ERROR: failed to download split metadata: {exc}", file=sys.stderr)
        return 2

    per_word = Counter()
    per_intent = Counter()
    scanned = 0

    for row in iter_split_rows(tar_path, wanted_splits):
        scanned += 1
        if args.valid_only:
            # field in MSWC split files is usually "is_valid"
            is_valid = pick_field(row, "is_valid", "IS_VALID", "valid", "VALID").strip().lower()
            if is_valid not in ("1", "true", "yes"):
                continue

        kw = norm_token(pick_field(row, "word", "WORD", "keyword", "KEYWORD"))
        if kw in target_words:
            per_word[kw] += 1
            per_intent[word_to_intent[kw]] += 1

    print(f"Scanned rows: {scanned}")
    print(f"Splits: {', '.join(sorted(wanted_splits))}")
    print(f"Metadata archive: {tar_path}")
    print("")
    print("Per-intent matches:")
    for intent in mapping.keys():
        print(f"  {intent:12s} {per_intent[intent]:8d}")

    print("")
    print("Per-word matches:")
    missing = []
    for intent, words in mapping.items():
        for w in words:
            c = per_word[w]
            print(f"  {intent:12s} {w:16s} {c:8d}")
            if c == 0:
                missing.append((intent, w))

    if missing:
        print("")
        print("Missing words (0 matches):")
        for intent, w in missing:
            print(f"  {intent:12s} {w}")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
