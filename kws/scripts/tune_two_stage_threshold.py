#!/usr/bin/env python3
"""
Sweep reject threshold for two-stage pipeline and pick best threshold.

Objective:
  score = intent_macro_f1 + alpha * unknown_recall
Subject to:
  known_recall >= min_known_recall
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Dict, List, Optional

import torch

from eval_two_stage import TwoStageDataset, evaluate_two_stage, make_loader, pick_split
from train_kws import KWSModel, read_manifest


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--clips-root", type=Path, required=True)
    p.add_argument("--reject-checkpoint", type=Path, required=True)
    p.add_argument("--intent-checkpoint", type=Path, required=True)
    p.add_argument("--run-dir", type=Path, required=True)
    p.add_argument("--split", default="test", choices=["train", "validation", "dev", "val", "test"])
    p.add_argument("--stage2-word-map", type=Path, default=None)
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--workers", type=int, default=0)
    p.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda"])
    p.add_argument("--th-min", type=float, default=0.10)
    p.add_argument("--th-max", type=float, default=0.90)
    p.add_argument("--th-step", type=float, default=0.02)
    p.add_argument("--alpha-unknown-recall", type=float, default=0.30)
    p.add_argument("--min-known-recall", type=float, default=0.90)
    args = p.parse_args()

    rows = read_manifest(args.manifest)
    rows_split = pick_split(rows, args.split)
    if not rows_split:
        raise RuntimeError(f"No rows for split={args.split}")

    reject_ckpt = torch.load(args.reject_checkpoint, map_location="cpu")
    intent_ckpt = torch.load(args.intent_checkpoint, map_location="cpu")

    reject_labels = list(reject_ckpt.get("labels", ["unknown", "known"]))
    if "known" not in reject_labels:
        raise RuntimeError("Reject checkpoint missing 'known' label")
    reject_known_idx = reject_labels.index("known")

    intent_labels = list(intent_ckpt.get("labels", []))
    if not intent_labels:
        raise RuntimeError("Intent checkpoint has no labels")

    word_to_intent: Dict[str, str] = {}
    if args.stage2_word_map is not None:
        raw = json.loads(args.stage2_word_map.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            raise RuntimeError("stage2-word-map must be JSON object")
        for k, v in raw.items():
            if isinstance(k, str) and isinstance(v, str):
                kk = k.strip()
                vv = v.strip()
                if kk and vv:
                    word_to_intent[kk] = vv

    if word_to_intent:
        final_labels = sorted({v for v in word_to_intent.values() if v not in ("unknown", "silence")})
    else:
        final_labels = [x for x in intent_labels if x not in ("unknown", "silence")]
        if "unknown" in final_labels:
            final_labels.remove("unknown")
    final_labels.append("unknown")
    final_label_to_idx = {name: i for i, name in enumerate(final_labels)}

    ds = TwoStageDataset(rows_split, args.clips_root, final_label_to_idx)
    if args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    loader = make_loader(ds, args.batch_size, args.workers, pin_memory=bool(device.type == "cuda"))

    reject_model = KWSModel(num_classes=len(reject_labels), width=int(reject_ckpt.get("width", 20))).to(device)
    reject_model.load_state_dict(reject_ckpt["model_state"])
    intent_model = KWSModel(num_classes=len(intent_labels), width=int(intent_ckpt.get("width", 24))).to(device)
    intent_model.load_state_dict(intent_ckpt["model_state"])

    rows_out: List[dict] = []
    best: Optional[dict] = None

    th = args.th_min
    while th <= args.th_max + 1e-9:
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

        per = res["per_class"]
        intents = [r for r in per if r["label"] != "unknown"]
        intent_macro_f1 = sum(r["f1"] for r in intents) / len(intents) if intents else 0.0
        reject = res["reject_metrics"]
        known_recall = float(reject["known_recall"])
        unknown_recall = float(reject["unknown_recall"])
        score = intent_macro_f1 + float(args.alpha_unknown_recall) * unknown_recall

        row = {
            "threshold": round(th, 4),
            "accuracy": float(res["accuracy"]),
            "macro_f1": float(res["macro_f1"]),
            "intent_macro_f1": float(intent_macro_f1),
            "known_recall": known_recall,
            "unknown_recall": unknown_recall,
            "score": float(score),
        }
        rows_out.append(row)

        if known_recall >= float(args.min_known_recall):
            if best is None or row["score"] > best["score"]:
                best = row

        th += args.th_step

    if best is None:
        # fallback: no threshold satisfies known_recall constraint
        best = max(rows_out, key=lambda x: x["score"]) if rows_out else {
            "threshold": 0.32,
            "accuracy": 0.0,
            "macro_f1": 0.0,
            "intent_macro_f1": 0.0,
            "known_recall": 0.0,
            "unknown_recall": 0.0,
            "score": 0.0,
        }

    args.run_dir.mkdir(parents=True, exist_ok=True)
    out_csv = args.run_dir / f"threshold_sweep_{args.split}.csv"
    out_json = args.run_dir / f"threshold_best_{args.split}.json"

    with out_csv.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(
            f,
            fieldnames=[
                "threshold",
                "accuracy",
                "macro_f1",
                "intent_macro_f1",
                "known_recall",
                "unknown_recall",
                "score",
            ],
        )
        w.writeheader()
        w.writerows(rows_out)

    out = {
        "split": args.split,
        "alpha_unknown_recall": args.alpha_unknown_recall,
        "min_known_recall": args.min_known_recall,
        "best": best,
        "sweep_rows": len(rows_out),
    }
    out_json.write_text(json.dumps(out, ensure_ascii=False, indent=2), encoding="utf-8")

    print(
        f"best threshold={best['threshold']:.4f} "
        f"score={best['score']:.4f} "
        f"acc={best['accuracy']:.4f} intent_f1={best['intent_macro_f1']:.4f} "
        f"known_rec={best['known_recall']:.4f} unknown_rec={best['unknown_recall']:.4f}"
    )
    print(f"Saved: {out_csv}")
    print(f"Saved: {out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

