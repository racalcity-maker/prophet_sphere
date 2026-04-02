#!/usr/bin/env python3
"""
Evaluate KWS checkpoint with per-class metrics and confusion matrix.

Example:
  python kws/scripts/eval_kws.py \
    --manifest kws/manifests/kws_balanced_manifest.csv \
    --clips-root D:/mswc_ru_clips \
    --checkpoint kws/runs/kws_ru_v1/kws_best.pt \
    --run-dir kws/runs/kws_ru_v1 \
    --split test
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Dict, List

import torch
from torch.utils.data import DataLoader

from train_kws import KWSDataset, KWSModel, Row, build_labels, read_manifest


def pick_split(rows: List[Row], split: str) -> List[Row]:
    s = split.strip().lower()
    if s == "train":
        return [r for r in rows if r.split == "train"]
    if s in ("validation", "val", "dev"):
        return [r for r in rows if r.split in ("validation", "val", "dev")]
    if s == "test":
        return [r for r in rows if r.split == "test"]
    raise ValueError(f"Unsupported split: {split}")


def make_loader(dataset: KWSDataset, batch_size: int, workers: int) -> DataLoader:
    return DataLoader(
        dataset,
        batch_size=batch_size,
        shuffle=False,
        num_workers=workers,
        pin_memory=True,
        drop_last=False,
    )


@torch.no_grad()
def evaluate_with_confusion(
    model: torch.nn.Module,
    loader: DataLoader,
    device: torch.device,
    num_classes: int,
    unknown_idx: int,
    unknown_threshold: float,
) -> tuple[float, List[List[int]]]:
    model.eval()
    total = 0
    correct = 0
    cm = [[0 for _ in range(num_classes)] for _ in range(num_classes)]  # true x pred

    for wav, y in loader:
        wav = wav.to(device, non_blocking=True)
        y = y.to(device, non_blocking=True)
        logits = model(wav)
        probs = torch.softmax(logits, dim=1)
        pred = torch.argmax(probs, dim=1)
        if unknown_threshold >= 0.0:
            conf = torch.max(probs, dim=1).values
            pred = torch.where(conf < unknown_threshold, torch.full_like(pred, unknown_idx), pred)

        total += int(y.numel())
        correct += int((pred == y).sum().item())
        y_cpu = y.cpu().tolist()
        p_cpu = pred.cpu().tolist()
        for yt, yp in zip(y_cpu, p_cpu):
            cm[int(yt)][int(yp)] += 1

    acc = (correct / total) if total > 0 else 0.0
    return acc, cm


def per_class_metrics(labels: List[str], cm: List[List[int]]) -> List[dict]:
    n = len(labels)
    rows: List[dict] = []
    for i in range(n):
        tp = cm[i][i]
        fn = sum(cm[i][j] for j in range(n) if j != i)
        fp = sum(cm[j][i] for j in range(n) if j != i)
        support = tp + fn
        precision = (tp / (tp + fp)) if (tp + fp) > 0 else 0.0
        recall = (tp / support) if support > 0 else 0.0
        f1 = (2.0 * precision * recall / (precision + recall)) if (precision + recall) > 0 else 0.0
        rows.append(
            {
                "label": labels[i],
                "support": support,
                "tp": tp,
                "fp": fp,
                "fn": fn,
                "precision": precision,
                "recall": recall,
                "f1": f1,
            }
        )
    return rows


def save_confusion_csv(path: Path, labels: List[str], cm: List[List[int]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["true\\pred", *labels])
        for i, label in enumerate(labels):
            w.writerow([label, *cm[i]])


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--clips-root", type=Path, required=True)
    p.add_argument("--checkpoint", type=Path, required=True)
    p.add_argument("--run-dir", type=Path, required=True)
    p.add_argument("--split", default="test", choices=["train", "validation", "dev", "val", "test"])
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--workers", type=int, default=0)
    p.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda"])
    p.add_argument("--unknown-threshold", type=float, default=-1.0, help="If >=0, force label=unknown when max softmax < threshold")
    args = p.parse_args()

    rows = read_manifest(args.manifest)
    if not rows:
        raise RuntimeError("Manifest is empty")
    split_rows = pick_split(rows, args.split)
    if not split_rows:
        raise RuntimeError(f"No rows for split={args.split}")

    ckpt = torch.load(args.checkpoint, map_location="cpu")
    labels = list(ckpt.get("labels") or build_labels(rows))
    width = int(ckpt.get("width", 24))

    label_to_idx: Dict[str, int] = {name: i for i, name in enumerate(labels)}
    ds = KWSDataset(split_rows, label_to_idx, args.clips_root, train_mode=False)
    loader = make_loader(ds, args.batch_size, args.workers)

    if args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)

    model = KWSModel(num_classes=len(labels), width=width).to(device)
    model.load_state_dict(ckpt["model_state"])

    unknown_idx = labels.index("unknown") if "unknown" in labels else -1
    if args.unknown_threshold >= 0.0 and unknown_idx < 0:
        raise RuntimeError("unknown-threshold was set but 'unknown' label is missing")
    acc, cm = evaluate_with_confusion(
        model,
        loader,
        device,
        num_classes=len(labels),
        unknown_idx=unknown_idx,
        unknown_threshold=args.unknown_threshold,
    )
    cls_rows = per_class_metrics(labels, cm)
    macro_f1 = sum(r["f1"] for r in cls_rows) / len(cls_rows)
    weighted_f1_num = sum(r["f1"] * r["support"] for r in cls_rows)
    weighted_f1_den = sum(r["support"] for r in cls_rows)
    weighted_f1 = (weighted_f1_num / weighted_f1_den) if weighted_f1_den > 0 else 0.0

    out_json = args.run_dir / f"eval_{args.split}.json"
    out_cm_csv = args.run_dir / f"confusion_{args.split}.csv"

    result = {
        "split": args.split,
        "device": str(device),
        "checkpoint": str(args.checkpoint),
        "num_samples": len(split_rows),
        "accuracy": acc,
        "macro_f1": macro_f1,
        "weighted_f1": weighted_f1,
        "labels": labels,
        "unknown_threshold": float(args.unknown_threshold),
        "per_class": cls_rows,
    }
    out_json.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    save_confusion_csv(out_cm_csv, labels, cm)

    print(f"split={args.split} samples={len(split_rows)} acc={acc:.4f} macro_f1={macro_f1:.4f} weighted_f1={weighted_f1:.4f}")
    print(f"Saved: {out_json}")
    print(f"Saved: {out_cm_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
