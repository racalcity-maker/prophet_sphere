#!/usr/bin/env python3
"""
Train binary reject model (known vs unknown) for two-stage KWS.

Known:
- all intent labels (non-unknown, non-silence)

Unknown:
- unknown label
- silence label (by default)

Example:
  python kws/scripts/train_reject.py \
    --manifest kws/manifests_v2/kws_balanced_manifest.csv \
    --clips-root D:/mswc_ru_clips \
    --out-dir kws/runs/reject_ru_v1 \
    --epochs 20 \
    --workers 0
"""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import torch
import torch.nn as nn
from torch.utils.data import DataLoader, WeightedRandomSampler

from train_kws import KWSModel, KWSDataset, Row, read_manifest, set_seed, split_rows


def to_binary_rows(rows: Sequence[Row], silence_as_unknown: bool) -> List[Row]:
    out: List[Row] = []
    for r in rows:
        label = "known"
        if r.label == "unknown":
            label = "unknown"
        elif r.label == "silence" and silence_as_unknown:
            label = "unknown"
        elif r.label == "silence" and not silence_as_unknown:
            label = "known"
        out.append(Row(split=r.split, label=label, sample_type=r.sample_type, link=r.link))
    return out


def counts(rows: Sequence[Row]) -> Counter:
    c = Counter()
    for r in rows:
        c[r.label] += 1
    return c


def make_loader(
    ds: KWSDataset,
    batch_size: int,
    workers: int,
    train_mode: bool,
    sampler: Optional[WeightedRandomSampler],
    pin_memory: bool,
) -> DataLoader:
    return DataLoader(
        ds,
        batch_size=batch_size,
        shuffle=(train_mode and sampler is None),
        sampler=sampler,
        num_workers=workers,
        pin_memory=pin_memory,
        drop_last=False,
    )


@torch.no_grad()
def collect_probs(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device,
    known_idx: int,
) -> Tuple[torch.Tensor, torch.Tensor]:
    model.eval()
    all_probs: List[torch.Tensor] = []
    all_targets: List[torch.Tensor] = []
    for wav, y in loader:
        wav = wav.to(device, non_blocking=True)
        y = y.to(device, non_blocking=True)
        logits = model(wav)
        probs = torch.softmax(logits, dim=1)[:, known_idx]
        all_probs.append(probs.detach().cpu())
        all_targets.append(y.detach().cpu())
    if not all_probs:
        return torch.empty(0), torch.empty(0, dtype=torch.long)
    return torch.cat(all_probs), torch.cat(all_targets)


def f1(precision: float, recall: float) -> float:
    return (2.0 * precision * recall / (precision + recall)) if (precision + recall) > 0 else 0.0


def metrics_for_threshold(
    probs_known: torch.Tensor,
    y_true: torch.Tensor,
    known_idx: int,
    th: float,
) -> dict:
    y_pred = torch.where(probs_known >= th, torch.full_like(y_true, known_idx), torch.zeros_like(y_true))
    acc = float((y_pred == y_true).float().mean().item()) if y_true.numel() > 0 else 0.0

    def cls_stats(cls: int) -> dict:
        tp = int(((y_pred == cls) & (y_true == cls)).sum().item())
        fn = int(((y_pred != cls) & (y_true == cls)).sum().item())
        fp = int(((y_pred == cls) & (y_true != cls)).sum().item())
        support = tp + fn
        precision = (tp / (tp + fp)) if (tp + fp) > 0 else 0.0
        recall = (tp / support) if support > 0 else 0.0
        return {
            "tp": tp,
            "fp": fp,
            "fn": fn,
            "support": support,
            "precision": precision,
            "recall": recall,
            "f1": f1(precision, recall),
        }

    unknown_stats = cls_stats(0 if known_idx == 1 else 1)
    known_stats = cls_stats(known_idx)
    macro_f1 = 0.5 * (unknown_stats["f1"] + known_stats["f1"])
    return {
        "threshold": th,
        "acc": acc,
        "macro_f1": macro_f1,
        "unknown": unknown_stats,
        "known": known_stats,
    }


def pick_best_threshold(
    probs_known: torch.Tensor,
    y_true: torch.Tensor,
    known_idx: int,
    t_min: float = 0.05,
    t_max: float = 0.95,
    t_step: float = 0.01,
) -> dict:
    best: Optional[dict] = None
    t = t_min
    while t <= t_max + 1e-9:
        m = metrics_for_threshold(probs_known, y_true, known_idx=known_idx, th=t)
        if best is None or (m["macro_f1"] > best["macro_f1"]) or (
            m["macro_f1"] == best["macro_f1"] and m["acc"] > best["acc"]
        ):
            best = m
        t += t_step
    if best is None:
        best = metrics_for_threshold(probs_known, y_true, known_idx=known_idx, th=0.5)
    return best


def train_one_epoch(
    model: nn.Module,
    loader: DataLoader,
    optimizer: torch.optim.Optimizer,
    ce: nn.Module,
    device: torch.device,
) -> float:
    model.train()
    total = 0
    loss_sum = 0.0
    for wav, y in loader:
        wav = wav.to(device, non_blocking=True)
        y = y.to(device, non_blocking=True)
        logits = model(wav)
        loss = ce(logits, y)

        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        optimizer.step()

        n = int(y.numel())
        total += n
        loss_sum += float(loss.item()) * n
    return (loss_sum / total) if total > 0 else 0.0


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--clips-root", type=Path, required=True)
    p.add_argument("--out-dir", type=Path, required=True)
    p.add_argument("--epochs", type=int, default=20)
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--lr", type=float, default=1e-3)
    p.add_argument("--width", type=int, default=20)
    p.add_argument("--workers", type=int, default=0)
    p.add_argument("--seed", type=int, default=1337)
    p.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda"])
    p.add_argument("--silence-as-unknown", action="store_true", default=True)
    p.add_argument("--no-silence-as-unknown", action="store_true", default=False)
    p.add_argument("--balanced-sampler", action="store_true", default=True)
    p.add_argument("--no-balanced-sampler", action="store_true", default=False)
    p.add_argument("--class-weighting", action="store_true", default=True)
    p.add_argument("--no-class-weighting", action="store_true", default=False)
    args = p.parse_args()

    if args.no_silence_as_unknown:
        args.silence_as_unknown = False
    if args.no_balanced_sampler:
        args.balanced_sampler = False
    if args.no_class_weighting:
        args.class_weighting = False

    set_seed(args.seed)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rows = read_manifest(args.manifest)
    if not rows:
        raise RuntimeError("Manifest is empty")
    rows_bin = to_binary_rows(rows, silence_as_unknown=bool(args.silence_as_unknown))
    train_rows, val_rows, test_rows = split_rows(rows_bin)
    if not train_rows or not val_rows:
        raise RuntimeError("Manifest must contain train and validation rows")

    labels = ["unknown", "known"]
    label_to_idx: Dict[str, int] = {"unknown": 0, "known": 1}
    known_idx = label_to_idx["known"]

    train_ds = KWSDataset(train_rows, label_to_idx, args.clips_root, train_mode=True)
    val_ds = KWSDataset(val_rows, label_to_idx, args.clips_root, train_mode=False)
    test_ds = KWSDataset(test_rows, label_to_idx, args.clips_root, train_mode=False) if test_rows else None

    if args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    pin_mem = bool(device.type == "cuda")

    c_train = counts(train_rows)
    sampler: Optional[WeightedRandomSampler] = None
    if args.balanced_sampler:
        sample_weights = []
        for r in train_rows:
            n = max(1, int(c_train.get(r.label, 1)))
            sample_weights.append(1.0 / float(n))
        sampler = WeightedRandomSampler(
            weights=torch.tensor(sample_weights, dtype=torch.double),
            num_samples=len(sample_weights),
            replacement=True,
        )

    train_loader = make_loader(
        train_ds,
        batch_size=args.batch_size,
        workers=args.workers,
        train_mode=True,
        sampler=sampler,
        pin_memory=pin_mem,
    )
    val_loader = make_loader(
        val_ds,
        batch_size=args.batch_size,
        workers=args.workers,
        train_mode=False,
        sampler=None,
        pin_memory=pin_mem,
    )
    test_loader = (
        make_loader(
            test_ds,
            batch_size=args.batch_size,
            workers=args.workers,
            train_mode=False,
            sampler=None,
            pin_memory=pin_mem,
        )
        if test_ds is not None
        else None
    )

    model = KWSModel(num_classes=2, width=args.width).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr)
    if args.class_weighting:
        w_unknown = 1.0 / max(1.0, float(c_train.get("unknown", 1)))
        w_known = 1.0 / max(1.0, float(c_train.get("known", 1)))
        w = torch.tensor([w_unknown, w_known], dtype=torch.float32)
        w = w / w.mean()
        ce = nn.CrossEntropyLoss(weight=w.to(device))
    else:
        ce = nn.CrossEntropyLoss()

    best_score = -1.0
    best_val: Optional[dict] = None
    history: List[dict] = []
    best_ckpt = args.out_dir / "reject_best.pt"

    for epoch in range(1, args.epochs + 1):
        train_loss = train_one_epoch(model, train_loader, optimizer, ce, device)
        probs_val, y_val = collect_probs(model, val_loader, device, known_idx=known_idx)
        best_val_this = pick_best_threshold(probs_val, y_val, known_idx=known_idx)
        rec = {
            "epoch": epoch,
            "train_loss": train_loss,
            "val_acc": best_val_this["acc"],
            "val_macro_f1": best_val_this["macro_f1"],
            "val_threshold": best_val_this["threshold"],
        }
        history.append(rec)
        print(
            f"[{epoch:03d}/{args.epochs:03d}] train_loss={train_loss:.4f} "
            f"val_macro_f1={best_val_this['macro_f1']:.4f} "
            f"val_acc={best_val_this['acc']:.4f} th={best_val_this['threshold']:.2f}"
        )

        if best_val_this["macro_f1"] > best_score:
            best_score = best_val_this["macro_f1"]
            best_val = best_val_this
            torch.save(
                {
                    "model_state": model.state_dict(),
                    "labels": labels,
                    "known_idx": known_idx,
                    "width": args.width,
                    "sample_rate": 16000,
                    "n_mels": 40,
                    "best_threshold": float(best_val_this["threshold"]),
                },
                best_ckpt,
            )

    report = {
        "labels": labels,
        "known_idx": known_idx,
        "seed": args.seed,
        "epochs": args.epochs,
        "batch_size": args.batch_size,
        "lr": args.lr,
        "width": args.width,
        "device": str(device),
        "silence_as_unknown": bool(args.silence_as_unknown),
        "balanced_sampler": bool(args.balanced_sampler),
        "class_weighting": bool(args.class_weighting),
        "train_counts": dict(c_train),
        "history": history,
        "best_val": best_val,
    }

    if best_ckpt.exists():
        ckpt = torch.load(best_ckpt, map_location=device)
        model.load_state_dict(ckpt["model_state"])
        best_th = float(ckpt.get("best_threshold", 0.5))
    else:
        best_th = 0.5

    if test_loader is not None:
        probs_test, y_test = collect_probs(model, test_loader, device, known_idx=known_idx)
        test_m = metrics_for_threshold(probs_test, y_test, known_idx=known_idx, th=best_th)
        report["test"] = test_m
        print(
            f"test_macro_f1={test_m['macro_f1']:.4f} "
            f"test_acc={test_m['acc']:.4f} th={best_th:.2f} "
            f"known_rec={test_m['known']['recall']:.4f} unknown_rec={test_m['unknown']['recall']:.4f}"
        )

    (args.out_dir / "reject_report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    (args.out_dir / "labels.json").write_text(json.dumps(labels, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Saved best model: {best_ckpt}")
    print(f"Saved report:     {args.out_dir / 'reject_report.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

