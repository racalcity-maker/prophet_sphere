#!/usr/bin/env python3
"""
Train a compact KWS model from balanced manifest.

Input CSV format (from build_balanced_kws_manifest.py):
  split,label,sample_type,intent,word,speaker,gender,link,hf_relpath

Supported sample types:
  - intent
  - unknown
  - synthetic_silence

Example:
  python kws/scripts/train_kws.py \
    --manifest kws/manifests/kws_balanced_manifest.csv \
    --clips-root D:/mswc_ru_clips \
    --out-dir kws/runs/kws_ru_v1
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader, Dataset, WeightedRandomSampler


def set_seed(seed: int) -> None:
    random.seed(seed)
    torch.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)


@dataclass
class Row:
    split: str
    label: str
    sample_type: str
    link: str


def read_manifest(path: Path) -> List[Row]:
    rows: List[Row] = []
    with path.open("r", encoding="utf-8", newline="") as f:
        r = csv.DictReader(f)
        for item in r:
            rows.append(
                Row(
                    split=str(item.get("split", "")).strip().lower(),
                    label=str(item.get("label", "")).strip(),
                    sample_type=str(item.get("sample_type", "")).strip(),
                    link=str(item.get("link", "")).strip(),
                )
            )
    return rows


def split_rows(rows: Sequence[Row]) -> Tuple[List[Row], List[Row], List[Row]]:
    train = [r for r in rows if r.split == "train"]
    val = [r for r in rows if r.split in ("validation", "dev", "val")]
    test = [r for r in rows if r.split == "test"]
    return train, val, test


def build_labels(rows: Sequence[Row]) -> List[str]:
    labels = sorted({r.label for r in rows if r.label})
    # Keep unknown/silence at the end if present.
    ordered: List[str] = []
    for x in labels:
        if x not in ("unknown", "silence"):
            ordered.append(x)
    if "unknown" in labels:
        ordered.append("unknown")
    if "silence" in labels:
        ordered.append("silence")
    return ordered


def rows_label_counts(rows: Sequence[Row]) -> Counter:
    c = Counter()
    for r in rows:
        if r.label:
            c[r.label] += 1
    return c


def build_class_weights(
    labels: Sequence[str],
    counts: Counter,
    unknown_loss_scale: float,
    silence_loss_scale: float,
) -> torch.Tensor:
    vals: List[float] = []
    for label in labels:
        n = max(1, int(counts.get(label, 1)))
        w = 1.0 / float(n)
        if label == "unknown":
            w *= max(0.01, unknown_loss_scale)
        elif label == "silence":
            w *= max(0.01, silence_loss_scale)
        vals.append(w)

    t = torch.tensor(vals, dtype=torch.float32)
    t = t / t.mean()
    return t


def pad_or_trim_1s(x: torch.Tensor, sample_rate: int) -> torch.Tensor:
    target = sample_rate
    n = x.numel()
    if n == target:
        return x
    if n > target:
        return x[:target]
    out = torch.zeros(target, dtype=x.dtype)
    out[:n] = x
    return out


def apply_time_shift(x: torch.Tensor, max_shift: int) -> torch.Tensor:
    if max_shift <= 0:
        return x
    shift = random.randint(-max_shift, max_shift)
    if shift == 0:
        return x
    out = torch.zeros_like(x)
    if shift > 0:
        out[shift:] = x[:-shift]
    else:
        out[:shift] = x[-shift:]
    return out


def apply_gain_and_noise(x: torch.Tensor, noise_scale: float) -> torch.Tensor:
    gain = random.uniform(0.7, 1.3)
    y = x * gain
    if noise_scale > 0:
        y = y + torch.randn_like(y) * random.uniform(0.0, noise_scale)
    return torch.clamp(y, -1.0, 1.0)


class KWSDataset(Dataset):
    def __init__(
        self,
        rows: Sequence[Row],
        label_to_idx: Dict[str, int],
        clips_root: Path,
        sample_rate: int = 16000,
        train_mode: bool = False,
        noise_scale: float = 0.015,
        shift_ms: int = 15,
    ) -> None:
        self.rows = list(rows)
        self.label_to_idx = label_to_idx
        self.clips_root = clips_root
        self.sample_rate = sample_rate
        self.train_mode = train_mode
        self.noise_scale = noise_scale
        self.shift_samples = int((shift_ms / 1000.0) * sample_rate)
        self._resamplers: Dict[int, torch.nn.Module] = {}
        self._torchaudio: Optional[object] = None
        self._have_torchaudio = False
        try:
            import torchaudio  # type: ignore

            self._torchaudio = torchaudio
            self._have_torchaudio = True
        except Exception:
            self._torchaudio = None
            self._have_torchaudio = False

    def __len__(self) -> int:
        return len(self.rows)

    def _resample(self, wav: torch.Tensor, sr: int) -> torch.Tensor:
        if sr == self.sample_rate:
            return wav

        if self._have_torchaudio and self._torchaudio is not None:
            key = sr
            if key not in self._resamplers:
                self._resamplers[key] = self._torchaudio.transforms.Resample(orig_freq=sr, new_freq=self.sample_rate)
            return self._resamplers[key](wav.unsqueeze(0)).squeeze(0)

        # Fallback without torchaudio resamplers.
        src = wav.unsqueeze(0).unsqueeze(0)  # [1, 1, T]
        dst_len = int(round((wav.numel() * self.sample_rate) / sr))
        out = F.interpolate(src, size=dst_len, mode="linear", align_corners=False)
        return out.squeeze(0).squeeze(0)

    def _load_audio(self, link: str) -> torch.Tensor:
        path = self.clips_root / link
        try:
            import soundfile as sf  # type: ignore

            arr, sr = sf.read(str(path), dtype="float32", always_2d=False)
            wav = torch.as_tensor(arr, dtype=torch.float32)
            if wav.ndim == 2:
                wav = wav.mean(dim=1)
        except Exception:
            if not self._have_torchaudio or self._torchaudio is None:
                raise
            wav_t, sr = self._torchaudio.load(str(path))
            if wav_t.ndim == 2:
                wav = wav_t.mean(dim=0)
            else:
                wav = wav_t.squeeze(0)

        wav = self._resample(wav, int(sr))

        wav = pad_or_trim_1s(wav, self.sample_rate)
        return wav

    def __getitem__(self, idx: int) -> Tuple[torch.Tensor, int]:
        row = self.rows[idx]
        y = self.label_to_idx[row.label]

        if row.sample_type == "synthetic_silence":
            wav = torch.randn(self.sample_rate) * 0.001
        else:
            wav = self._load_audio(row.link)
            if self.train_mode:
                wav = apply_time_shift(wav, self.shift_samples)
                wav = apply_gain_and_noise(wav, self.noise_scale)

        return wav, y


class MelFrontend(nn.Module):
    def __init__(self, sample_rate: int = 16000, n_mels: int = 40) -> None:
        super().__init__()
        import torchaudio

        self.mel = torchaudio.transforms.MelSpectrogram(
            sample_rate=sample_rate,
            n_fft=512,
            win_length=400,
            hop_length=160,
            f_min=20.0,
            f_max=7600.0,
            n_mels=n_mels,
            power=2.0,
        )

    def forward(self, wav: torch.Tensor) -> torch.Tensor:
        # wav: [B, T]
        x = self.mel(wav)  # [B, M, Frames]
        x = torch.log(x + 1e-6)
        x = (x - x.mean(dim=(1, 2), keepdim=True)) / (x.std(dim=(1, 2), keepdim=True) + 1e-5)
        return x.unsqueeze(1)  # [B, 1, M, Frames]


class DSConvBlock(nn.Module):
    def __init__(self, channels: int) -> None:
        super().__init__()
        self.depthwise = nn.Conv2d(channels, channels, kernel_size=3, stride=1, padding=1, groups=channels, bias=False)
        self.dw_bn = nn.BatchNorm2d(channels)
        self.pointwise = nn.Conv2d(channels, channels, kernel_size=1, stride=1, padding=0, bias=False)
        self.pw_bn = nn.BatchNorm2d(channels)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = F.relu(self.dw_bn(self.depthwise(x)), inplace=True)
        x = F.relu(self.pw_bn(self.pointwise(x)), inplace=True)
        return x


class KWSModel(nn.Module):
    def __init__(self, num_classes: int, width: int = 24) -> None:
        super().__init__()
        self.front = MelFrontend(sample_rate=16000, n_mels=40)
        self.stem = nn.Sequential(
            nn.Conv2d(1, width, kernel_size=3, stride=2, padding=1, bias=False),
            nn.BatchNorm2d(width),
            nn.ReLU(inplace=True),
        )
        self.blocks = nn.Sequential(
            DSConvBlock(width),
            DSConvBlock(width),
            DSConvBlock(width),
            DSConvBlock(width),
        )
        self.head = nn.Linear(width, num_classes)

    def forward(self, wav: torch.Tensor) -> torch.Tensor:
        x = self.front(wav)
        x = self.stem(x)
        x = self.blocks(x)
        x = x.mean(dim=(2, 3))
        return self.head(x)


def make_loader(
    dataset: Dataset,
    batch_size: int,
    train_mode: bool,
    workers: int,
    sampler: Optional[WeightedRandomSampler] = None,
    pin_memory: bool = False,
) -> DataLoader:
    return DataLoader(
        dataset,
        batch_size=batch_size,
        shuffle=(train_mode and sampler is None),
        sampler=sampler,
        num_workers=workers,
        pin_memory=pin_memory,
        drop_last=False,
    )


@torch.no_grad()
def evaluate(model: nn.Module, loader: DataLoader, device: torch.device) -> Tuple[float, float]:
    model.eval()
    total = 0
    correct = 0
    loss_sum = 0.0
    ce = nn.CrossEntropyLoss()

    for wav, y in loader:
        wav = wav.to(device, non_blocking=True)
        y = y.to(device, non_blocking=True)
        logits = model(wav)
        loss = ce(logits, y)
        pred = torch.argmax(logits, dim=1)
        correct += int((pred == y).sum().item())
        total += int(y.numel())
        loss_sum += float(loss.item()) * int(y.numel())

    if total == 0:
        return 0.0, 0.0
    return loss_sum / total, correct / total


def train_one_epoch(
    model: nn.Module,
    loader: DataLoader,
    optimizer: torch.optim.Optimizer,
    device: torch.device,
    ce: nn.Module,
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
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--clips-root", type=Path, required=True, help="Root directory for MSWC clips; expects <root>/<word>/<file>.opus")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--width", type=int, default=24)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda"])
    parser.add_argument("--balanced-sampler", action="store_true", default=False)
    parser.add_argument("--class-weighting", action="store_true", default=False)
    parser.add_argument("--unknown-loss-scale", type=float, default=0.35)
    parser.add_argument("--silence-loss-scale", type=float, default=0.7)
    args = parser.parse_args()

    set_seed(args.seed)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rows = read_manifest(args.manifest)
    if not rows:
        raise RuntimeError("Manifest is empty")

    labels = build_labels(rows)
    label_to_idx = {name: i for i, name in enumerate(labels)}

    train_rows, val_rows, test_rows = split_rows(rows)
    if not train_rows or not val_rows:
        raise RuntimeError("Manifest must contain train and validation rows")

    train_ds = KWSDataset(train_rows, label_to_idx, args.clips_root, train_mode=True)
    val_ds = KWSDataset(val_rows, label_to_idx, args.clips_root, train_mode=False)
    test_ds = KWSDataset(test_rows, label_to_idx, args.clips_root, train_mode=False) if test_rows else None

    if args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)

    pin_mem = bool(device.type == "cuda")
    train_counts = rows_label_counts(train_rows)

    train_sampler: Optional[WeightedRandomSampler] = None
    if args.balanced_sampler:
        sample_weights = []
        for r in train_rows:
            n = max(1, int(train_counts.get(r.label, 1)))
            sample_weights.append(1.0 / float(n))
        train_sampler = WeightedRandomSampler(
            weights=torch.tensor(sample_weights, dtype=torch.double),
            num_samples=len(sample_weights),
            replacement=True,
        )

    train_loader = make_loader(
        train_ds,
        args.batch_size,
        train_mode=True,
        workers=args.workers,
        sampler=train_sampler,
        pin_memory=pin_mem,
    )
    val_loader = make_loader(val_ds, args.batch_size, train_mode=False, workers=args.workers, pin_memory=pin_mem)
    test_loader = (
        make_loader(test_ds, args.batch_size, train_mode=False, workers=args.workers, pin_memory=pin_mem)
        if test_ds is not None
        else None
    )

    model = KWSModel(num_classes=len(labels), width=args.width).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr)
    if args.class_weighting:
        class_weights = build_class_weights(
            labels=labels,
            counts=train_counts,
            unknown_loss_scale=args.unknown_loss_scale,
            silence_loss_scale=args.silence_loss_scale,
        ).to(device)
        ce_train: nn.Module = nn.CrossEntropyLoss(weight=class_weights)
    else:
        ce_train = nn.CrossEntropyLoss()

    best_val_acc = -1.0
    best_path = args.out_dir / "kws_best.pt"
    history: List[dict] = []

    for epoch in range(1, args.epochs + 1):
        train_loss = train_one_epoch(model, train_loader, optimizer, device, ce_train)
        val_loss, val_acc = evaluate(model, val_loader, device)

        rec = {
            "epoch": epoch,
            "train_loss": train_loss,
            "val_loss": val_loss,
            "val_acc": val_acc,
        }
        history.append(rec)
        print(
            f"[{epoch:03d}/{args.epochs:03d}] "
            f"train_loss={train_loss:.4f} "
            f"val_loss={val_loss:.4f} val_acc={val_acc:.4f}"
        )

        if val_acc > best_val_acc:
            best_val_acc = val_acc
            torch.save(
                {
                    "model_state": model.state_dict(),
                    "labels": labels,
                    "width": args.width,
                    "sample_rate": 16000,
                    "n_mels": 40,
                },
                best_path,
            )

    final = {
        "labels": labels,
        "seed": args.seed,
        "epochs": args.epochs,
        "batch_size": args.batch_size,
        "lr": args.lr,
        "width": args.width,
        "device": str(device),
        "best_val_acc": best_val_acc,
        "train_rows": len(train_rows),
        "val_rows": len(val_rows),
        "test_rows": len(test_rows),
        "history": history,
        "balanced_sampler": bool(args.balanced_sampler),
        "class_weighting": bool(args.class_weighting),
        "unknown_loss_scale": float(args.unknown_loss_scale),
        "silence_loss_scale": float(args.silence_loss_scale),
        "train_label_counts": dict(train_counts),
    }

    if best_path.exists():
        ckpt = torch.load(best_path, map_location=device)
        model.load_state_dict(ckpt["model_state"])

    if test_loader is not None:
        test_loss, test_acc = evaluate(model, test_loader, device)
        final["test_loss"] = test_loss
        final["test_acc"] = test_acc
        print(f"test_loss={test_loss:.4f} test_acc={test_acc:.4f}")

    (args.out_dir / "train_report.json").write_text(
        json.dumps(final, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    (args.out_dir / "labels.json").write_text(json.dumps(labels, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Saved best model: {best_path}")
    print(f"Saved report:     {args.out_dir / 'train_report.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
