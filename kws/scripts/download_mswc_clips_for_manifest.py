#!/usr/bin/env python3
"""
Download only the clip files referenced in a KWS manifest.

MSWC stores audio in split archives:
  data/<format>/<lang>/<split>/audio/<n>.tar.gz
and metadata links are in form:
  <word>/<filename>.opus

This script downloads archives per split and extracts only requested files.
"""

from __future__ import annotations

import argparse
import csv
import tarfile
from pathlib import Path
from typing import Dict, Iterable, Set, Tuple


def canonical_split_name(split: str) -> str:
    s = split.strip().lower()
    if s in ("validation", "valid", "val", "dev"):
        return "dev"
    if s == "test":
        return "test"
    return "train"


def archive_member_name_from_link(link: str) -> str:
    # In MSWC archive files are flattened: <word>/<file>.opus -> <word>_<file>.opus
    return link.replace("/", "_")


def read_manifest(manifest: Path) -> Tuple[Set[str], Dict[str, Dict[str, str]]]:
    all_links: Set[str] = set()
    by_split: Dict[str, Dict[str, str]] = {"train": {}, "dev": {}, "test": {}}
    with manifest.open("r", encoding="utf-8", newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            sample_type = str(row.get("sample_type", "")).strip()
            if sample_type == "synthetic_silence":
                continue
            link = str(row.get("link", "")).strip()
            if not link:
                continue
            link = link.replace("\\", "/")
            split = canonical_split_name(str(row.get("split", "")))
            member = archive_member_name_from_link(link)
            all_links.add(link)
            by_split.setdefault(split, {})[member] = link
    return all_links, by_split


def read_n_files(path: Path) -> int:
    txt = path.read_text(encoding="utf-8", errors="ignore").strip()
    return int(txt)


def iter_tar_members(tar_path: Path) -> Iterable[Tuple[str, tarfile.TarInfo]]:
    with tarfile.open(tar_path, "r:gz") as tf:
        for member in tf.getmembers():
            if not member.isfile():
                continue
            name = member.name.replace("\\", "/")
            base = Path(name).name
            yield base, member


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--out-root", type=Path, required=True, help="Directory where clips will be stored as <word>/<file>.opus")
    parser.add_argument("--repo", default="MLCommons/ml_spoken_words")
    parser.add_argument("--lang", default="ru")
    parser.add_argument("--format", default="opus", choices=["opus", "wav"])
    parser.add_argument("--revision", default=None)
    args = parser.parse_args()

    from huggingface_hub import hf_hub_download  # type: ignore

    links, links_by_split = read_manifest(args.manifest)
    links = sorted(links)
    if not links:
        print("No links found in manifest")
        return 0

    args.out_root.mkdir(parents=True, exist_ok=True)

    total = len(links)
    ok = 0
    failed = 0

    # Pre-count existing files
    pending_by_split: Dict[str, Dict[str, str]] = {"train": {}, "dev": {}, "test": {}}
    for split, member_map in links_by_split.items():
        for member_name, link in member_map.items():
            dest = args.out_root / Path(link)
            if dest.exists() and dest.stat().st_size > 0:
                ok += 1
            else:
                pending_by_split.setdefault(split, {})[member_name] = link

    for split in ("train", "dev", "test"):
        pending = pending_by_split.get(split, {})
        if not pending:
            continue

        n_files_remote = f"data/{args.format}/{args.lang}/{split}/n_files.txt"
        try:
            n_files_path = Path(
                hf_hub_download(
                    repo_id=args.repo,
                    repo_type="dataset",
                    filename=n_files_remote,
                    revision=args.revision,
                )
            )
            n_files = read_n_files(n_files_path)
        except Exception as exc:
            failed += len(pending)
            print(f"[{split}] FAIL cannot read n_files.txt ({n_files_remote}): {exc}")
            for link in pending.values():
                print(f"  unresolved: {link}")
            continue

        print(f"[{split}] scanning {n_files} archive shards for {len(pending)} requested clips...")
        for i in range(n_files):
            if not pending:
                break
            remote_archive = f"data/{args.format}/{args.lang}/{split}/audio/{i}.tar.gz"
            try:
                local_archive = Path(
                    hf_hub_download(
                        repo_id=args.repo,
                        repo_type="dataset",
                        filename=remote_archive,
                        revision=args.revision,
                    )
                )
            except Exception as exc:
                print(f"  shard {i}/{n_files - 1} download failed: {exc}")
                continue

            try:
                with tarfile.open(local_archive, "r:gz") as tf:
                    for member in tf.getmembers():
                        if not member.isfile():
                            continue
                        base = Path(member.name.replace("\\", "/")).name
                        link = pending.get(base)
                        if not link:
                            continue
                        src = tf.extractfile(member)
                        if src is None:
                            continue
                        dest = args.out_root / Path(link)
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        with dest.open("wb") as out_f:
                            out_f.write(src.read())
                        ok += 1
                        del pending[base]
            except Exception as exc:
                print(f"  shard {i}/{n_files - 1} read failed: {exc}")
                continue

            if (i + 1) % 10 == 0 or not pending:
                done = total - (sum(len(v) for v in pending_by_split.values()))
                print(f"  progress split={split}: shard={i + 1}/{n_files}, remaining={len(pending)}, total_done={done}/{total}")

        if pending:
            failed += len(pending)
            print(f"[{split}] unresolved clips: {len(pending)}")
            for link in list(pending.values())[:30]:
                print(f"  - {link}")
            if len(pending) > 30:
                print(f"  ... and {len(pending) - 30} more")

    print(f"Done. total={total} ok={ok} failed={failed} out_root={args.out_root}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
