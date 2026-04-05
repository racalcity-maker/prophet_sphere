#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Dict, List


def _read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _scan_answer_files(root: Path) -> Dict[str, List[str]]:
    answers_root = root / "answers"
    out: Dict[str, List[str]] = {}
    if not answers_root.exists():
        return out
    for domain_dir in answers_root.iterdir():
        if not domain_dir.is_dir():
            continue
        domain = domain_dir.name.strip().lower()
        stems = sorted([p.stem.strip().lower() for p in domain_dir.glob("*.json") if p.is_file()])
        out[domain] = stems
    return out


def sync(root: Path) -> dict:
    dict_dir = root / "dictionaries"
    sub_path = dict_dir / "subintents.json"
    int_path = dict_dir / "intents.json"
    if not sub_path.exists():
        raise FileNotFoundError(f"missing {sub_path}")
    if not int_path.exists():
        raise FileNotFoundError(f"missing {int_path}")

    answer_map = _scan_answer_files(root)
    sub = _read_json(sub_path)
    ints = _read_json(int_path)

    sub_total = 0
    sub_enabled = 0
    sub_disabled: List[str] = []
    for domain, items in (sub.get("subintents") or {}).items():
        if not isinstance(items, list):
            continue
        existing = set(answer_map.get(str(domain).strip().lower(), []))
        for item in items:
            if not isinstance(item, dict):
                continue
            sub_total += 1
            sid = str(item.get("id", "")).strip().lower()
            if not sid:
                item["enabled"] = False
                item.pop("answer_file", None)
                sub_disabled.append(f"{domain}:<empty>")
                continue
            ok = sid in existing
            item["enabled"] = bool(ok)
            if ok:
                sub_enabled += 1
                item["answer_file"] = f"answers/{domain}/{sid}.json"
            else:
                item.pop("answer_file", None)
                sub_disabled.append(f"{domain}:{sid}")

    intents_enabled: List[str] = []
    intents_disabled: List[str] = []
    for rec in (ints.get("intents") or []):
        if not isinstance(rec, dict):
            continue
        iid = str(rec.get("id", "")).strip().lower()
        if not iid:
            continue
        enabled = bool(answer_map.get(iid)) or iid in ("general",)
        rec["enabled"] = enabled
        if enabled:
            intents_enabled.append(iid)
        else:
            intents_disabled.append(iid)

    _write_json(sub_path, sub)
    _write_json(int_path, ints)

    return {
        "domains_with_answers": sorted(answer_map.keys()),
        "subintents_total": sub_total,
        "subintents_enabled": sub_enabled,
        "subintents_disabled": sorted(sub_disabled),
        "intents_enabled": sorted(intents_enabled),
        "intents_disabled": sorted(intents_disabled),
    }


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Sync oracle dictionaries with answers/* files")
    p.add_argument(
        "--root",
        default="docs/oracle_texts_jsons",
        help="Oracle texts JSON root directory",
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()
    report = sync(root)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

