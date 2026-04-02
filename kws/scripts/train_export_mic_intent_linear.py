#!/usr/bin/env python3
"""
Train and export a lightweight intent classifier for on-device mic inference.

Model:
  - Input: 6 handcrafted features from 1s/16k PCM capture
  - Classifier: single linear layer (multinomial logistic regression)
  - Output: intent classes + unknown

Export:
  components/services_mic/include/mic_intent_weights.h
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple

import soundfile as sf
import torch
import torch.nn as nn
import torch.nn.functional as F

SAMPLE_RATE = 16000
TARGET_SAMPLES = 16000
FEATURE_DIM = 6

INTENT_ORDER = [
    "unknown",
    "love",
    "future",
    "choice",
    "money",
    "path",
    "danger",
    "inner_state",
    "wish",
    "yes_no",
]

INTENT_TO_ENUM = {
    "unknown": "ORB_INTENT_UNKNOWN",
    "love": "ORB_INTENT_LOVE",
    "future": "ORB_INTENT_FUTURE",
    "choice": "ORB_INTENT_CHOICE",
    "money": "ORB_INTENT_MONEY",
    "path": "ORB_INTENT_PATH",
    "danger": "ORB_INTENT_DANGER",
    "inner_state": "ORB_INTENT_INNER_STATE",
    "wish": "ORB_INTENT_WISH",
    "yes_no": "ORB_INTENT_YES_NO",
}


@dataclass
class Row:
    split: str
    sample_type: str
    intent: str
    link: str


def split_norm(split: str) -> str:
    s = split.strip().lower()
    if s in ("validation", "val", "dev"):
        return "validation"
    return s


def read_rows(manifest: Path) -> List[Row]:
    rows: List[Row] = []
    with manifest.open("r", encoding="utf-8", newline="") as f:
        r = csv.DictReader(f)
        for item in r:
            rows.append(
                Row(
                    split=split_norm(str(item.get("split", ""))),
                    sample_type=str(item.get("sample_type", "")).strip().lower(),
                    intent=str(item.get("intent", "")).strip().lower(),
                    link=str(item.get("link", "")).strip(),
                )
            )
    return rows


def resample_1d(wav: torch.Tensor, src_rate: int, dst_rate: int) -> torch.Tensor:
    if src_rate == dst_rate:
        return wav
    src = wav.unsqueeze(0).unsqueeze(0)
    dst_len = int(round((wav.numel() * dst_rate) / src_rate))
    out = F.interpolate(src, size=dst_len, mode="linear", align_corners=False)
    return out.squeeze(0).squeeze(0)


def pad_trim_1s(wav: torch.Tensor) -> torch.Tensor:
    n = wav.numel()
    if n == TARGET_SAMPLES:
        return wav
    if n > TARGET_SAMPLES:
        return wav[:TARGET_SAMPLES]
    out = torch.zeros(TARGET_SAMPLES, dtype=wav.dtype)
    out[:n] = wav
    return out


def load_wav(path: Path) -> torch.Tensor:
    arr, sr = sf.read(str(path), dtype="float32", always_2d=False)
    wav = torch.as_tensor(arr, dtype=torch.float32)
    if wav.ndim == 2:
        wav = wav.mean(dim=1)
    wav = torch.clamp(wav, -1.0, 1.0)
    wav = resample_1d(wav, int(sr), SAMPLE_RATE)
    wav = pad_trim_1s(wav)
    return wav


def to_pcm16(wav: torch.Tensor) -> torch.Tensor:
    x = torch.clamp(wav * 32767.0, -32768.0, 32767.0)
    return x.to(torch.int16)


def extract_features_pcm16(pcm16: torch.Tensor) -> torch.Tensor:
    # Mirrors runtime feature logic in mic_intent_model.c
    s_count = int(pcm16.numel())
    if s_count <= 0:
        return torch.zeros(FEATURE_DIM, dtype=torch.float32)

    abs_x = torch.abs(pcm16.to(torch.int32))
    avg_abs = int(abs_x.float().mean().item())
    peak = int(abs_x.max().item())

    x = pcm16.to(torch.int32)
    signs = (x >= 0).to(torch.int32)
    zc = int((signs[1:] != signs[:-1]).sum().item()) if s_count > 1 else 0

    lp_state = 0
    env_state = 0
    low_abs_sum = 0
    high_abs_sum = 0
    impulsive_count = 0
    for xi in x.tolist():
        lp_state += (xi - lp_state) >> 3
        low = lp_state
        high = xi - low
        low_abs_sum += abs(low)
        high_abs_sum += abs(high)

        ax = abs(xi)
        env_state += (ax - env_state) >> 4
        if ax > (env_state + 1200):
            impulsive_count += 1

    band_sum = low_abs_sum + high_abs_sum
    hf = int((high_abs_sum * 1000) / band_sum) if band_sum > 0 else 0
    crest = int((peak * 1000) / avg_abs) if avg_abs > 0 else 0
    impulse = int((impulsive_count * 1000) / s_count)

    avg_abs_pm = min(1000, int((avg_abs * 1000) / 4096))
    peak_rel_pm = min(1000, int((peak * 1000) / 32767))
    zcr_pm = min(1000, int((zc * 1000) / s_count))
    hf_pm = min(1000, hf)
    crest_pm = min(1000, crest)
    impulse_pm = min(1000, impulse)

    return torch.tensor(
        [avg_abs_pm, peak_rel_pm, zcr_pm, hf_pm, crest_pm, impulse_pm],
        dtype=torch.float32,
    ) / 1000.0


def normalize_intent(sample_type: str, intent: str) -> str:
    if sample_type == "synthetic_silence":
        return "unknown"
    if intent == "luck":
        intent = "path"
    if intent in INTENT_TO_ENUM:
        return intent
    return "unknown"


def build_dataset(rows: List[Row], clips_root: Path) -> Dict[str, Tuple[torch.Tensor, torch.Tensor]]:
    xs: Dict[str, List[torch.Tensor]] = {"train": [], "validation": [], "test": []}
    ys: Dict[str, List[int]] = {"train": [], "validation": [], "test": []}
    intent_to_idx = {name: i for i, name in enumerate(INTENT_ORDER)}

    for row in rows:
        split = row.split
        if split not in xs:
            continue
        label = normalize_intent(row.sample_type, row.intent)
        y = intent_to_idx[label]

        if row.sample_type == "synthetic_silence":
            wav = (torch.randn(TARGET_SAMPLES) * 0.0008).to(torch.float32)
        else:
            path = clips_root / row.link
            if not path.exists():
                continue
            wav = load_wav(path)

        feats = extract_features_pcm16(to_pcm16(wav))
        xs[split].append(feats)
        ys[split].append(y)

    out: Dict[str, Tuple[torch.Tensor, torch.Tensor]] = {}
    for split in ("train", "validation", "test"):
        if xs[split]:
            out[split] = (torch.stack(xs[split], dim=0), torch.tensor(ys[split], dtype=torch.long))
        else:
            out[split] = (torch.zeros((0, FEATURE_DIM), dtype=torch.float32), torch.zeros((0,), dtype=torch.long))
    return out


def class_weights(y: torch.Tensor, num_classes: int) -> torch.Tensor:
    counts = torch.bincount(y, minlength=num_classes).float()
    counts = torch.clamp(counts, min=1.0)
    w = 1.0 / counts
    w = w / w.mean()
    return w


def accuracy(logits: torch.Tensor, y: torch.Tensor) -> float:
    if y.numel() == 0:
        return 0.0
    pred = torch.argmax(logits, dim=1)
    return float((pred == y).float().mean().item())


def export_header(path: Path, w: torch.Tensor, b: torch.Tensor) -> None:
    lines: List[str] = []
    lines.append("#ifndef MIC_INTENT_WEIGHTS_H")
    lines.append("#define MIC_INTENT_WEIGHTS_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("#include \"app_defs.h\"")
    lines.append("")
    lines.append("#define MIC_INTENT_FEATURE_DIM 6")
    lines.append(f"#define MIC_INTENT_CLASS_COUNT {len(INTENT_ORDER)}")
    lines.append("")
    lines.append("static const orb_intent_id_t s_mic_intent_class_to_id[MIC_INTENT_CLASS_COUNT] = {")
    for name in INTENT_ORDER:
        lines.append(f"    {INTENT_TO_ENUM[name]},")
    lines.append("};")
    lines.append("")
    lines.append("static const float s_mic_intent_linear_weights[MIC_INTENT_CLASS_COUNT][MIC_INTENT_FEATURE_DIM] = {")
    for i in range(w.shape[0]):
        vals = ", ".join(f"{float(v):.9f}f" for v in w[i].tolist())
        lines.append(f"    {{ {vals} }},")
    lines.append("};")
    lines.append("")
    lines.append("static const float s_mic_intent_linear_bias[MIC_INTENT_CLASS_COUNT] = {")
    lines.append("    " + ", ".join(f"{float(v):.9f}f" for v in b.tolist()))
    lines.append("};")
    lines.append("")
    lines.append("#define MIC_INTENT_MIN_CONF_PERMILLE 560U")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=Path("kws/manifests_word_v2/kws_word_manifest.csv"))
    parser.add_argument("--clips-root", type=Path, default=Path("kws/data/mswc_ru_word_v2"))
    parser.add_argument("--out-header", type=Path, default=Path("components/services_mic/include/mic_intent_weights.h"))
    parser.add_argument("--out-report", type=Path, default=Path("kws/runs/mic_intent_linear_v1_report.json"))
    parser.add_argument("--epochs", type=int, default=700)
    parser.add_argument("--lr", type=float, default=0.04)
    parser.add_argument("--seed", type=int, default=1337)
    args = parser.parse_args()

    torch.manual_seed(args.seed)

    rows = read_rows(args.manifest)
    data = build_dataset(rows, args.clips_root)
    x_train, y_train = data["train"]
    x_val, y_val = data["validation"]
    x_test, y_test = data["test"]
    if x_train.numel() == 0:
        raise RuntimeError("empty train set for mic intent model")

    model = nn.Linear(FEATURE_DIM, len(INTENT_ORDER))
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    ce = nn.CrossEntropyLoss(weight=class_weights(y_train, len(INTENT_ORDER)))

    best_state = None
    best_val = -1.0
    history = []

    for epoch in range(1, args.epochs + 1):
        model.train()
        logits = model(x_train)
        loss = ce(logits, y_train)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()

        model.eval()
        with torch.no_grad():
            val_logits = model(x_val)
            val_acc = accuracy(val_logits, y_val)
            train_acc = accuracy(model(x_train), y_train)
        history.append({"epoch": epoch, "loss": float(loss.item()), "train_acc": train_acc, "val_acc": val_acc})
        if val_acc >= best_val:
            best_val = val_acc
            best_state = {
                "w": model.weight.detach().clone(),
                "b": model.bias.detach().clone(),
            }

    if best_state is None:
        raise RuntimeError("training failed: no best state")

    with torch.no_grad():
        model.weight.copy_(best_state["w"])
        model.bias.copy_(best_state["b"])
        test_acc = accuracy(model(x_test), y_test)

    export_header(args.out_header, model.weight.detach(), model.bias.detach())

    report = {
        "manifest": str(args.manifest),
        "clips_root": str(args.clips_root),
        "train_samples": int(y_train.numel()),
        "val_samples": int(y_val.numel()),
        "test_samples": int(y_test.numel()),
        "best_val_acc": best_val,
        "test_acc": test_acc,
        "classes": INTENT_ORDER,
        "epochs": args.epochs,
        "lr": args.lr,
        "seed": args.seed,
        "history_tail": history[-20:],
    }
    args.out_report.parent.mkdir(parents=True, exist_ok=True)
    args.out_report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"exported header: {args.out_header}")
    print(f"report:          {args.out_report}")
    print(f"best_val_acc={best_val:.4f} test_acc={test_acc:.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
