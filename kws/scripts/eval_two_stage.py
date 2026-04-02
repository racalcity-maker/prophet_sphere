#!/usr/bin/env python3
"""
Evaluate two-stage pipeline:
  1) reject model (known vs unknown)
  2) intent model (only if stage1 says known)

Ground truth mapping for evaluation:
  - intent labels -> same intent
  - unknown / silence -> unknown
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Dict, List, Tuple

import torch
from torch.utils.data import DataLoader, Dataset

from train_kws import KWSModel, KWSDataset, Row, read_manifest


def pick_split(rows: List[Row], split: str) -> List[Row]:
    s = split.strip().lower()
    if s == "train":
        return [r for r in rows if r.split == "train"]
    if s in ("validation", "val", "dev"):
        return [r for r in rows if r.split in ("validation", "val", "dev")]
    if s == "test":
        return [r for r in rows if r.split == "test"]
    raise ValueError(f"Unsupported split: {split}")


class TwoStageDataset(Dataset):
    def __init__(self, rows: List[Row], clips_root: Path, final_label_to_idx: Dict[str, int]) -> None:
        self.rows = rows
        self.final_label_to_idx = final_label_to_idx
        # Reuse KWSDataset audio loading and synthetic_silence generation.
        dummy_rows = [Row(split=r.split, label="_", sample_type=r.sample_type, link=r.link) for r in rows]
        self.audio_ds = KWSDataset(dummy_rows, {"_": 0}, clips_root, train_mode=False)

    def __len__(self) -> int:
        return len(self.rows)

    def __getitem__(self, idx: int) -> Tuple[torch.Tensor, int]:
        wav, _ = self.audio_ds[idx]
        gt_label = self.rows[idx].label
        if gt_label in ("unknown", "silence"):
            gt_label = "unknown"
        y = self.final_label_to_idx[gt_label]
        return wav, y


def make_loader(ds: Dataset, batch_size: int, workers: int, pin_memory: bool) -> DataLoader:
    return DataLoader(
        ds,
        batch_size=batch_size,
        shuffle=False,
        num_workers=workers,
        pin_memory=pin_memory,
        drop_last=False,
    )


def per_class_metrics(labels: List[str], cm: List[List[int]]) -> List[dict]:
    n = len(labels)
    out: List[dict] = []
    for i in range(n):
        tp = cm[i][i]
        fn = sum(cm[i][j] for j in range(n) if j != i)
        fp = sum(cm[j][i] for j in range(n) if j != i)
        support = tp + fn
        precision = (tp / (tp + fp)) if (tp + fp) > 0 else 0.0
        recall = (tp / support) if support > 0 else 0.0
        f1 = (2.0 * precision * recall / (precision + recall)) if (precision + recall) > 0 else 0.0
        out.append(
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
    return out


def save_confusion_csv(path: Path, labels: List[str], cm: List[List[int]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["true\\pred", *labels])
        for i, label in enumerate(labels):
            w.writerow([label, *cm[i]])


@torch.no_grad()
def evaluate_two_stage(
    reject_model: torch.nn.Module,
    intent_model: torch.nn.Module,
    loader: DataLoader,
    device: torch.device,
    reject_known_idx: int,
    reject_threshold: float,
    intent_labels: List[str],
    final_label_to_idx: Dict[str, int],
    word_to_intent: Dict[str, str],
) -> dict:
    reject_model.eval()
    intent_model.eval()

    final_labels = list(final_label_to_idx.keys())
    n = len(final_labels)
    cm = [[0 for _ in range(n)] for _ in range(n)]
    total = 0
    correct = 0

    unknown_idx_final = final_label_to_idx["unknown"]
    reject_y_true: List[int] = []
    reject_y_pred: List[int] = []

    for wav, y_true_final in loader:
        wav = wav.to(device, non_blocking=True)
        y_true_final = y_true_final.to(device, non_blocking=True)

        # Stage 1: reject
        rej_logits = reject_model(wav)
        rej_probs = torch.softmax(rej_logits, dim=1)[:, reject_known_idx]
        stage1_known = rej_probs >= reject_threshold

        # Stage 2: intent (only for known)
        intent_logits = intent_model(wav)
        intent_pred_idx = torch.argmax(intent_logits, dim=1)

        pred_final = torch.full_like(y_true_final, unknown_idx_final)
        for i in range(int(wav.shape[0])):
            true_idx = int(y_true_final[i].item())
            true_is_known = int(true_idx != unknown_idx_final)
            pred_is_known = int(stage1_known[i].item())
            reject_y_true.append(true_is_known)
            reject_y_pred.append(pred_is_known)

            if pred_is_known == 0:
                pred_final[i] = unknown_idx_final
                continue

            stage2_label = intent_labels[int(intent_pred_idx[i].item())]
            intent_label = word_to_intent.get(stage2_label, stage2_label) if word_to_intent else stage2_label
            if intent_label in ("unknown", "silence", ""):
                pred_final[i] = unknown_idx_final
            else:
                pred_final[i] = final_label_to_idx.get(intent_label, unknown_idx_final)

        total += int(y_true_final.numel())
        correct += int((pred_final == y_true_final).sum().item())

        yt = y_true_final.cpu().tolist()
        yp = pred_final.cpu().tolist()
        for t, p in zip(yt, yp):
            cm[int(t)][int(p)] += 1

    accuracy = (correct / total) if total > 0 else 0.0
    cls = per_class_metrics(final_labels, cm)
    macro_f1 = sum(r["f1"] for r in cls) / len(cls)
    weighted_f1_num = sum(r["f1"] * r["support"] for r in cls)
    weighted_f1_den = sum(r["support"] for r in cls)
    weighted_f1 = (weighted_f1_num / weighted_f1_den) if weighted_f1_den > 0 else 0.0

    # Reject metrics (known=1, unknown=0)
    tp = sum(1 for t, p in zip(reject_y_true, reject_y_pred) if t == 1 and p == 1)
    fn = sum(1 for t, p in zip(reject_y_true, reject_y_pred) if t == 1 and p == 0)
    fp = sum(1 for t, p in zip(reject_y_true, reject_y_pred) if t == 0 and p == 1)
    tn = sum(1 for t, p in zip(reject_y_true, reject_y_pred) if t == 0 and p == 0)
    known_precision = (tp / (tp + fp)) if (tp + fp) > 0 else 0.0
    known_recall = (tp / (tp + fn)) if (tp + fn) > 0 else 0.0
    unknown_recall = (tn / (tn + fp)) if (tn + fp) > 0 else 0.0

    return {
        "accuracy": accuracy,
        "macro_f1": macro_f1,
        "weighted_f1": weighted_f1,
        "per_class": cls,
        "confusion": cm,
        "final_labels": final_labels,
        "reject_metrics": {
            "known_precision": known_precision,
            "known_recall": known_recall,
            "unknown_recall": unknown_recall,
            "tp": tp,
            "tn": tn,
            "fp": fp,
            "fn": fn,
        },
    }


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--clips-root", type=Path, required=True)
    p.add_argument("--reject-checkpoint", type=Path, required=True)
    p.add_argument("--intent-checkpoint", type=Path, required=True)
    p.add_argument("--run-dir", type=Path, required=True)
    p.add_argument("--split", default="test", choices=["train", "validation", "dev", "val", "test"])
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--workers", type=int, default=0)
    p.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda"])
    p.add_argument("--reject-threshold", type=float, default=-1.0, help="If < 0, use threshold stored in reject checkpoint")
    p.add_argument("--stage2-word-map", type=Path, default=None, help="Optional JSON mapping {word: intent} for stage2 word-model")
    args = p.parse_args()

    rows = read_manifest(args.manifest)
    rows_split = pick_split(rows, args.split)
    if not rows_split:
        raise RuntimeError(f"No rows for split={args.split}")

    reject_ckpt = torch.load(args.reject_checkpoint, map_location="cpu")
    intent_ckpt = torch.load(args.intent_checkpoint, map_location="cpu")

    reject_labels = list(reject_ckpt.get("labels", ["unknown", "known"]))
    if "known" not in reject_labels:
        raise RuntimeError("Reject checkpoint does not contain 'known' label")
    reject_known_idx = reject_labels.index("known")

    intent_labels = list(intent_ckpt.get("labels", []))
    if not intent_labels:
        raise RuntimeError("Intent checkpoint has no labels")

    word_to_intent: Dict[str, str] = {}
    if args.stage2_word_map is not None:
        raw = json.loads(args.stage2_word_map.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            raise RuntimeError("stage2-word-map must be JSON object {word: intent}")
        for k, v in raw.items():
            if isinstance(k, str) and isinstance(v, str):
                kk = k.strip()
                vv = v.strip()
                if kk and vv:
                    word_to_intent[kk] = vv

    # Final evaluated labels = intents + unknown
    if word_to_intent:
        final_labels = sorted({v for v in word_to_intent.values() if v not in ("unknown", "silence")})
    else:
        final_labels = [x for x in intent_labels if x not in ("unknown", "silence")]
        if "unknown" in final_labels:
            final_labels.remove("unknown")
    final_labels.append("unknown")
    final_label_to_idx = {name: i for i, name in enumerate(final_labels)}

    ds = TwoStageDataset(rows_split, args.clips_root, final_label_to_idx=final_label_to_idx)
    if args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    loader = make_loader(ds, args.batch_size, args.workers, pin_memory=bool(device.type == "cuda"))

    reject_model = KWSModel(num_classes=len(reject_labels), width=int(reject_ckpt.get("width", 20))).to(device)
    reject_model.load_state_dict(reject_ckpt["model_state"])
    intent_model = KWSModel(num_classes=len(intent_labels), width=int(intent_ckpt.get("width", 24))).to(device)
    intent_model.load_state_dict(intent_ckpt["model_state"])

    th = float(args.reject_threshold)
    if th < 0.0:
        th = float(reject_ckpt.get("best_threshold", 0.5))

    res = evaluate_two_stage(
        reject_model=reject_model,
        intent_model=intent_model,
        loader=loader,
        device=device,
        reject_known_idx=reject_known_idx,
        reject_threshold=th,
        intent_labels=intent_labels,
        final_label_to_idx=final_label_to_idx,
        word_to_intent=word_to_intent,
    )

    out_json = args.run_dir / f"eval_two_stage_{args.split}.json"
    out_cm = args.run_dir / f"confusion_two_stage_{args.split}.csv"
    out = {
        "split": args.split,
        "device": str(device),
        "manifest": str(args.manifest),
        "reject_checkpoint": str(args.reject_checkpoint),
        "intent_checkpoint": str(args.intent_checkpoint),
        "reject_threshold": th,
        "num_samples": len(rows_split),
        "accuracy": res["accuracy"],
        "macro_f1": res["macro_f1"],
        "weighted_f1": res["weighted_f1"],
        "final_labels": res["final_labels"],
        "per_class": res["per_class"],
        "reject_metrics": res["reject_metrics"],
    }
    args.run_dir.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(out, ensure_ascii=False, indent=2), encoding="utf-8")
    save_confusion_csv(out_cm, res["final_labels"], res["confusion"])

    print(
        f"two-stage split={args.split} samples={len(rows_split)} "
        f"acc={res['accuracy']:.4f} macro_f1={res['macro_f1']:.4f} "
        f"known_recall={res['reject_metrics']['known_recall']:.4f} "
        f"unknown_recall={res['reject_metrics']['unknown_recall']:.4f}"
    )
    print(f"Saved: {out_json}")
    print(f"Saved: {out_cm}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
