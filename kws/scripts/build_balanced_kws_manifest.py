#!/usr/bin/env python3
"""
Build balanced KWS train/validation/test manifest from MSWC RU metadata.

Inputs:
  - intent manifest (positive classes only)
  - archetype map (to know mapped words)
  - MSWC split metadata (for unknown sampling)

Output:
  - balanced manifest CSV with labels: 9 intents + unknown + silence
  - JSON stats summary
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import random
import tarfile
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Set


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


def iter_split_rows(tar_path: Path) -> Iterable[dict]:
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


def load_intent_manifest(path: Path) -> List[dict]:
    rows: List[dict] = []
    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            row["split"] = canonical_split_name(row.get("split", ""))
            row["intent"] = str(row.get("intent", ""))
            row["word"] = norm_token(str(row.get("word", "")))
            rows.append(row)
    return rows


@dataclass
class UnknownRow:
    split: str
    word: str
    speaker: str
    gender: str
    link: str
    hf_relpath: str


def collect_unknown_pool(
    tar_path: Path,
    mapped_words: Set[str],
    valid_only: bool,
    limit_per_word: int,
) -> Dict[str, List[UnknownRow]]:
    by_split: Dict[str, List[UnknownRow]] = {"train": [], "validation": [], "test": []}
    word_counter: Dict[str, Counter] = {
        "train": Counter(),
        "validation": Counter(),
        "test": Counter(),
    }

    for row in iter_split_rows(tar_path):
        split = row["_split"]
        valid_flag = pick_field(row, "VALID", "valid", "is_valid", "IS_VALID").strip().lower()
        if valid_only and valid_flag not in ("1", "true", "yes"):
            continue

        word = norm_token(pick_field(row, "WORD", "word", "KEYWORD", "keyword"))
        if not word or word in mapped_words:
            continue
        if word_counter[split][word] >= limit_per_word:
            continue
        word_counter[split][word] += 1

        link = pick_field(row, "LINK", "link")
        by_split[split].append(
            UnknownRow(
                split=split,
                word=word,
                speaker=pick_field(row, "SPEAKER", "speaker"),
                gender=pick_field(row, "GENDER", "gender"),
                link=link,
                hf_relpath=f"data/recordings/ru/clips/{link}",
            )
        )

    return by_split


def sample_rows(rng: random.Random, rows: Sequence[dict], n: int) -> List[dict]:
    if n <= 0:
        return []
    if len(rows) <= n:
        return list(rows)
    idx = list(range(len(rows)))
    rng.shuffle(idx)
    selected_idx = idx[:n]
    return [rows[i] for i in selected_idx]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--map", type=Path, default=Path("kws/archetypes_map.json"))
    parser.add_argument("--intent-manifest", type=Path, default=Path("kws/manifests/mswc_intent_manifest.csv"))
    parser.add_argument("--repo", default="MLCommons/ml_spoken_words")
    parser.add_argument("--lang", default="ru")
    parser.add_argument("--revision", default=None)
    parser.add_argument("--out-dir", type=Path, default=Path("kws/manifests"))
    parser.add_argument("--unknown-ratio", type=float, default=1.0)
    parser.add_argument("--silence-ratio", type=float, default=0.20)
    parser.add_argument("--unknown-max-per-word", type=int, default=12)
    parser.add_argument("--valid-only", action="store_true")
    parser.add_argument("--seed", type=int, default=1337)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    mapping = load_mapping(args.map)
    intents = list(mapping.keys())
    mapped_words = {w for words in mapping.values() for w in words}
    pos_rows = load_intent_manifest(args.intent_manifest)
    tar_path = download_splits_tar(args.repo, args.lang, args.revision)

    by_split_intent: Dict[str, Dict[str, List[dict]]] = {
        "train": {i: [] for i in intents},
        "validation": {i: [] for i in intents},
        "test": {i: [] for i in intents},
    }
    for row in pos_rows:
        split = row.get("split", "")
        intent = row.get("intent", "")
        if split in by_split_intent and intent in by_split_intent[split]:
            by_split_intent[split][intent].append(row)

    target_per_split: Dict[str, int] = {}
    for split in ("train", "validation", "test"):
        counts = [len(by_split_intent[split][i]) for i in intents]
        target_per_split[split] = min(counts) if counts else 0

    unknown_pool = collect_unknown_pool(
        tar_path=tar_path,
        mapped_words=mapped_words,
        valid_only=args.valid_only,
        limit_per_word=max(1, args.unknown_max_per_word),
    )

    out_rows: List[dict] = []
    label_counts: Dict[str, Counter] = {
        "train": Counter(),
        "validation": Counter(),
        "test": Counter(),
    }
    silence_ids: Dict[str, int] = {"train": 0, "validation": 0, "test": 0}

    for split in ("train", "validation", "test"):
        target = target_per_split[split]
        if target <= 0:
            continue

        # Positive intents balanced to same size.
        for intent in intents:
            selected = sample_rows(rng, by_split_intent[split][intent], target)
            for row in selected:
                out_rows.append(
                    {
                        "split": split,
                        "label": intent,
                        "sample_type": "intent",
                        "intent": intent,
                        "word": row["word"],
                        "speaker": row.get("speaker", ""),
                        "gender": row.get("gender", ""),
                        "link": row.get("link", ""),
                        "hf_relpath": row.get("hf_relpath", ""),
                    }
                )
                label_counts[split][intent] += 1

        base_intent_total = target * len(intents)
        unknown_n = int(round(base_intent_total * max(0.0, args.unknown_ratio)))
        silence_n = int(round(base_intent_total * max(0.0, args.silence_ratio)))

        # Unknown from non-mapped words.
        unknown_candidates = unknown_pool[split]
        rng.shuffle(unknown_candidates)
        unknown_selected = unknown_candidates[: min(unknown_n, len(unknown_candidates))]
        for item in unknown_selected:
            out_rows.append(
                {
                    "split": split,
                    "label": "unknown",
                    "sample_type": "unknown",
                    "intent": "",
                    "word": item.word,
                    "speaker": item.speaker,
                    "gender": item.gender,
                    "link": item.link,
                    "hf_relpath": item.hf_relpath,
                }
            )
            label_counts[split]["unknown"] += 1

        # Silence as synthetic slots (to generate at training time).
        for _ in range(silence_n):
            silence_ids[split] += 1
            sid = silence_ids[split]
            out_rows.append(
                {
                    "split": split,
                    "label": "silence",
                    "sample_type": "synthetic_silence",
                    "intent": "",
                    "word": "",
                    "speaker": f"silence_{sid:06d}",
                    "gender": "",
                    "link": "",
                    "hf_relpath": "",
                }
            )
            label_counts[split]["silence"] += 1

    args.out_dir.mkdir(parents=True, exist_ok=True)
    out_csv = args.out_dir / "kws_balanced_manifest.csv"
    out_json = args.out_dir / "kws_balanced_stats.json"

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

    stats = {
        "source_intent_manifest": str(args.intent_manifest),
        "source_tar": str(tar_path),
        "seed": args.seed,
        "target_per_split": target_per_split,
        "unknown_ratio": args.unknown_ratio,
        "silence_ratio": args.silence_ratio,
        "per_split_label_counts": {split: dict(cnt) for split, cnt in label_counts.items()},
        "rows_total": len(out_rows),
    }
    out_json.write_text(json.dumps(stats, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"Balanced manifest: {out_csv}")
    print(f"Stats:             {out_json}")
    print(f"Rows total:        {len(out_rows)}")
    for split in ("train", "validation", "test"):
        if label_counts[split]:
            print(f"[{split}]")
            for label, n in sorted(label_counts[split].items()):
                print(f"  {label:12s} {n:6d}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

