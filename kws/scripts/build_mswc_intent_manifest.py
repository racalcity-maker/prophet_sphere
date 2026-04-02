#!/usr/bin/env python3
"""
Build intent manifest from MLCommons MSWC RU split metadata.

Output:
  - CSV with positive intent samples found by archetype map
  - JSON stats by split/intent/word
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import tarfile
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, Iterable, List


def norm_token(text: str) -> str:
    token = text.strip().lower().replace("ё", "е")
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


def canonical_split_name(name: str) -> str:
    s = name.strip().lower()
    if s in ("validation", "valid", "val", "dev"):
        return "validation"
    return s


def pick_field(row: dict, *candidates: str) -> str:
    for key in candidates:
        if key in row and row[key] is not None:
            return str(row[key])
    return ""


def load_mapping(path: Path) -> Dict[str, List[str]]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    out: Dict[str, List[str]] = {}
    for intent, words in raw.items():
        if not isinstance(intent, str) or not isinstance(words, list):
            continue
        clean = [norm_token(w) for w in words if isinstance(w, str) and norm_token(w)]
        if clean:
            out[intent] = clean
    return out


def build_word_to_intent(mapping: Dict[str, List[str]]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for intent, words in mapping.items():
        for w in words:
            if w not in out:
                out[w] = intent
    return out


def download_splits_tar(repo_id: str, lang: str, revision: str | None) -> Path:
    from huggingface_hub import hf_hub_download  # type: ignore

    return Path(
        hf_hub_download(
            repo_id=repo_id,
            repo_type="dataset",
            filename=f"data/splits/{lang}/splits.tar.gz",
            revision=revision,
        )
    )


def iter_rows(tar_path: Path) -> Iterable[dict]:
    with tarfile.open(tar_path, "r:gz") as tf:
        for member in tf.getmembers():
            if not member.isfile() or not member.name.endswith(".csv"):
                continue
            split = canonical_split_name(Path(member.name).stem)
            if split not in ("train", "validation", "test"):
                continue
            fobj = tf.extractfile(member)
            if fobj is None:
                continue
            reader = csv.DictReader(io.TextIOWrapper(fobj, encoding="utf-8", newline=""))
            for row in reader:
                row["_split"] = split
                yield row


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--map", type=Path, default=Path("kws/archetypes_map.json"))
    parser.add_argument("--repo", default="MLCommons/ml_spoken_words")
    parser.add_argument("--lang", default="ru")
    parser.add_argument("--revision", default=None)
    parser.add_argument("--out-dir", type=Path, default=Path("kws/manifests"))
    parser.add_argument("--valid-only", action="store_true")
    args = parser.parse_args()

    mapping = load_mapping(args.map)
    word_to_intent = build_word_to_intent(mapping)
    target_words = set(word_to_intent.keys())

    tar_path = download_splits_tar(args.repo, args.lang, args.revision)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rows: List[dict] = []
    per_intent_split = defaultdict(Counter)
    per_word = Counter()

    for row in iter_rows(tar_path):
        if args.valid_only:
            valid = pick_field(row, "VALID", "valid", "is_valid", "IS_VALID").strip().lower()
            if valid not in ("1", "true", "yes"):
                continue

        word = norm_token(pick_field(row, "WORD", "word", "KEYWORD", "keyword"))
        if word not in target_words:
            continue

        intent = word_to_intent[word]
        split = canonical_split_name(str(row.get("_split", "")))
        link = pick_field(row, "LINK", "link")
        speaker = pick_field(row, "SPEAKER", "speaker")
        gender = pick_field(row, "GENDER", "gender")

        rows.append(
            {
                "split": split,
                "intent": intent,
                "word": word,
                "speaker": speaker,
                "gender": gender,
                "link": link,
                "hf_relpath": f"data/recordings/{args.lang}/clips/{link}",
            }
        )
        per_intent_split[split][intent] += 1
        per_word[word] += 1

    csv_path = args.out_dir / "mswc_intent_manifest.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(
            f,
            fieldnames=["split", "intent", "word", "speaker", "gender", "link", "hf_relpath"],
        )
        w.writeheader()
        w.writerows(rows)

    stats = {
        "rows_total": len(rows),
        "source_tar": str(tar_path),
        "per_split_intent": {split: dict(cnt) for split, cnt in per_intent_split.items()},
        "per_word": dict(per_word),
    }
    stats_path = args.out_dir / "mswc_intent_stats.json"
    stats_path.write_text(json.dumps(stats, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"Manifest: {csv_path}")
    print(f"Stats:    {stats_path}")
    print(f"Rows:     {len(rows)}")
    for split in ("train", "validation", "test"):
        if split in per_intent_split:
            print(f"[{split}]")
            for intent in mapping.keys():
                print(f"  {intent:12s} {per_intent_split[split][intent]:6d}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

