#!/usr/bin/env python3
"""
Build word-level manifest for stage-2 intent model.

Input:
  - kws/manifests/mswc_intent_manifest.csv (positive rows: intent + word + split + link)

Output:
  - CSV where label is WORD (not intent)
  - JSON stats, including selected words and word->intent map

Example:
  python kws/scripts/build_word_intent_manifest.py \
    --intent-manifest kws/manifests/mswc_intent_manifest.csv \
    --out-dir kws/manifests_word_v1 \
    --min-train-per-word 20 \
    --min-val-per-word 3 \
    --min-test-per-word 3 \
    --target-train-per-word 40 \
    --target-val-per-word 6 \
    --target-test-per-word 6
"""

from __future__ import annotations

import argparse
import csv
import json
import random
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, List, Sequence


def canonical_split(split: str) -> str:
    s = split.strip().lower()
    if s in ("validation", "valid", "val", "dev"):
        return "validation"
    return s


def sample_rows(rng: random.Random, rows: Sequence[dict], target: int) -> List[dict]:
    if target <= 0:
        return []
    if len(rows) <= target:
        return list(rows)
    idx = list(range(len(rows)))
    rng.shuffle(idx)
    idx = idx[:target]
    return [rows[i] for i in idx]


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--intent-manifest", type=Path, default=Path("kws/manifests/mswc_intent_manifest.csv"))
    p.add_argument("--out-dir", type=Path, default=Path("kws/manifests_word_v1"))
    p.add_argument("--seed", type=int, default=1337)
    p.add_argument("--min-train-per-word", type=int, default=20)
    p.add_argument("--min-val-per-word", type=int, default=3)
    p.add_argument("--min-test-per-word", type=int, default=3)
    p.add_argument("--target-train-per-word", type=int, default=40)
    p.add_argument("--target-val-per-word", type=int, default=6)
    p.add_argument("--target-test-per-word", type=int, default=6)
    p.add_argument("--max-words-per-intent", type=int, default=0, help="0 = keep all eligible words, >0 = keep top-N words by train count per intent")
    p.add_argument("--silence-ratio", type=float, default=0.10, help="Synthetic silence relative to total train word rows")
    p.add_argument(
        "--selected-word-map",
        type=Path,
        default=None,
        help="Optional JSON map intent->[words] for curated selection (applied before max-words-per-intent)",
    )
    args = p.parse_args()

    rng = random.Random(args.seed)
    rows: List[dict] = []
    with args.intent_manifest.open("r", encoding="utf-8", newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            split = canonical_split(str(row.get("split", "")))
            if split not in ("train", "validation", "test"):
                continue
            intent = str(row.get("intent", "")).strip()
            word = str(row.get("word", "")).strip()
            link = str(row.get("link", "")).strip()
            if not intent or not word or not link:
                continue
            rows.append(
                {
                    "split": split,
                    "intent": intent,
                    "word": word,
                    "speaker": str(row.get("speaker", "")).strip(),
                    "gender": str(row.get("gender", "")).strip(),
                    "link": link,
                    "hf_relpath": str(row.get("hf_relpath", "")).strip(),
                }
            )

    by_split_word: Dict[str, Dict[str, List[dict]]] = {
        "train": defaultdict(list),
        "validation": defaultdict(list),
        "test": defaultdict(list),
    }
    word_to_intent: Dict[str, str] = {}
    for r in rows:
        by_split_word[r["split"]][r["word"]].append(r)
        word_to_intent.setdefault(r["word"], r["intent"])

    words_all = sorted(set(by_split_word["train"].keys()) | set(by_split_word["validation"].keys()) | set(by_split_word["test"].keys()))
    selected_words: List[str] = []
    for w in words_all:
        if len(by_split_word["train"].get(w, [])) < args.min_train_per_word:
            continue
        if len(by_split_word["validation"].get(w, [])) < args.min_val_per_word:
            continue
        if len(by_split_word["test"].get(w, [])) < args.min_test_per_word:
            continue
        selected_words.append(w)

    if args.selected_word_map is not None:
        selected_word_map_raw = json.loads(args.selected_word_map.read_text(encoding="utf-8"))
        curated_words: List[str] = []
        for intent, words in selected_word_map_raw.items():
            if not isinstance(intent, str) or not isinstance(words, list):
                continue
            for w in words:
                if isinstance(w, str):
                    curated_words.append(w)
                    word_to_intent[w] = intent
        curated_words_set = set(curated_words)
        selected_words = [w for w in selected_words if w in curated_words_set]

    if args.max_words_per_intent > 0:
        by_intent: Dict[str, List[str]] = defaultdict(list)
        for w in selected_words:
            by_intent[word_to_intent[w]].append(w)
        pruned: List[str] = []
        for intent, words in by_intent.items():
            ranked = sorted(words, key=lambda x: len(by_split_word["train"].get(x, [])), reverse=True)
            pruned.extend(ranked[: args.max_words_per_intent])
        selected_words = sorted(pruned)

    out_rows: List[dict] = []
    cnt = {"train": Counter(), "validation": Counter(), "test": Counter()}
    targets = {
        "train": max(1, args.target_train_per_word),
        "validation": max(1, args.target_val_per_word),
        "test": max(1, args.target_test_per_word),
    }

    for split in ("train", "validation", "test"):
        for w in selected_words:
            selected = sample_rows(rng, by_split_word[split].get(w, []), targets[split])
            for r in selected:
                out_rows.append(
                    {
                        "split": split,
                        "label": w,  # WORD class
                        "sample_type": "intent_word",
                        "intent": r["intent"],
                        "word": r["word"],
                        "speaker": r["speaker"],
                        "gender": r["gender"],
                        "link": r["link"],
                        "hf_relpath": r["hf_relpath"],
                    }
                )
                cnt[split][w] += 1

    # Add synthetic silence only to train/validation/test in proportion to train word rows.
    for split in ("train", "validation", "test"):
        base = sum(cnt[split].values())
        n_sil = int(round(base * max(0.0, args.silence_ratio)))
        for i in range(n_sil):
            out_rows.append(
                {
                    "split": split,
                    "label": "silence",
                    "sample_type": "synthetic_silence",
                    "intent": "",
                    "word": "",
                    "speaker": f"silence_{split}_{i:06d}",
                    "gender": "",
                    "link": "",
                    "hf_relpath": "",
                }
            )
            cnt[split]["silence"] += 1

    args.out_dir.mkdir(parents=True, exist_ok=True)
    out_csv = args.out_dir / "kws_word_manifest.csv"
    out_stats = args.out_dir / "kws_word_stats.json"
    out_map = args.out_dir / "word_to_intent.json"

    with out_csv.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(
            f,
            fieldnames=[
                "split",
                "label",
                "sample_type",
                "intent",
                "word",
                "speaker",
                "gender",
                "link",
                "hf_relpath",
            ],
        )
        w.writeheader()
        w.writerows(out_rows)

    out_map.write_text(json.dumps({w: word_to_intent[w] for w in selected_words}, ensure_ascii=False, indent=2), encoding="utf-8")
    stats = {
        "source_intent_manifest": str(args.intent_manifest),
        "seed": args.seed,
        "selected_words": selected_words,
        "selected_word_count": len(selected_words),
        "targets": targets,
        "max_words_per_intent": args.max_words_per_intent,
        "min_requirements": {
            "train": args.min_train_per_word,
            "validation": args.min_val_per_word,
            "test": args.min_test_per_word,
        },
        "silence_ratio": args.silence_ratio,
        "per_split_counts": {s: dict(c) for s, c in cnt.items()},
        "rows_total": len(out_rows),
    }
    out_stats.write_text(json.dumps(stats, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"Word manifest: {out_csv}")
    print(f"Word->intent:  {out_map}")
    print(f"Stats:         {out_stats}")
    print(f"Selected words: {len(selected_words)}")
    for split in ("train", "validation", "test"):
        print(f"[{split}] rows={sum(cnt[split].values())}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
