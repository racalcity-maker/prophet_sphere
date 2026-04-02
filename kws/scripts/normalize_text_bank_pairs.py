#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Iterable, List


SECTION_HEADER_RE = re.compile(
    r"^\s*("
    r"Домен\s*[:,]|"
    r"Greeting\b|Understanding\b|Prediction\b|Farewell\b|"
    r"Положительные\b|Отрицательные\b|Нейтральные\b|"
    r"INTRO\b|RETRY\b|FAIL\b|JOKE\b|FORBIDDEN\b"
    r")",
    re.IGNORECASE,
)

SENTENCE_SPLIT_RE = re.compile(r"(?<=[.!?])\s+")


def is_header(line: str) -> bool:
    return bool(SECTION_HEADER_RE.match(line))


def normalize_section_boundaries(raw: str) -> str:
    text = raw.replace("\ufeff", "")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    header_tokens = [
        r"Домен\s*[:,]",
        r"Greeting\b",
        r"Understanding\b",
        r"Prediction\b",
        r"Положительные\b",
        r"Отрицательные\b",
        r"Нейтральные\b",
        r"Farewell\b",
        r"INTRO\b",
        r"RETRY\b",
        r"FAIL\b",
        r"JOKE\b",
        r"FORBIDDEN\b",
    ]
    for token in header_tokens:
        text = re.sub(rf"\s+({token})", r"\n\1", text, flags=re.IGNORECASE)
    return text


def split_sentences(text: str) -> List[str]:
    clean = re.sub(r"\s+", " ", text).strip()
    if not clean:
        return []
    parts = [p.strip() for p in SENTENCE_SPLIT_RE.split(clean) if p.strip()]
    return parts


def join_in_pairs(sentences: Iterable[str]) -> List[str]:
    items = list(sentences)
    out: List[str] = []
    i = 0
    while i < len(items):
        if i + 1 < len(items):
            out.append(f"{items[i]} {items[i + 1]}")
            i += 2
        else:
            out.append(items[i])
            i += 1
    return out


def normalize_file(path: Path) -> bool:
    raw = path.read_text(encoding="utf-8-sig", errors="ignore")
    prepared = normalize_section_boundaries(raw)
    lines = prepared.split("\n")

    if not "".join(lines).strip():
        return False

    out: List[str] = []
    block: List[str] = []
    changed = False

    def flush_block() -> None:
        nonlocal changed
        if not block:
            return
        text = " ".join(x.strip() for x in block if x.strip())
        block.clear()
        if not text:
            return
        sentences = split_sentences(text)
        pairs = join_in_pairs(sentences)
        out.extend(pairs)
        if len(pairs) != 1 or pairs[0] != text:
            changed = True

    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        if is_header(stripped):
            header = stripped
            remainder = ""
            m = re.match(r"^(.*?\))\s+(.+)$", stripped)
            if m:
                maybe_header = m.group(1).strip()
                if is_header(maybe_header):
                    header = maybe_header
                    remainder = m.group(2).strip()

            flush_block()
            if out and out[-1] != "":
                out.append("")
            out.append(header)
            if remainder:
                block.append(remainder)
            continue
        block.append(stripped)

    flush_block()

    while out and out[-1] == "":
        out.pop()

    normalized = "\n".join(out).strip() + "\n"
    if normalized != (raw.strip() + "\n"):
        changed = True
        path.write_text(normalized, encoding="utf-8")
    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description="Normalize text bank files into 2-sentence phrases.")
    parser.add_argument("--dir", default="docs/texts", help="Directory with *.txt text bank files")
    args = parser.parse_args()

    root = Path(args.dir)
    if not root.exists():
        print(f"missing directory: {root}")
        return 1

    changed_files = 0
    total = 0
    for txt in sorted(root.glob("*.txt")):
        total += 1
        if normalize_file(txt):
            changed_files += 1
            print(f"updated: {txt}")
        else:
            print(f"skip: {txt}")

    print(f"done: {changed_files}/{total} files updated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
