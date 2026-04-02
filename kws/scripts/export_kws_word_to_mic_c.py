#!/usr/bin/env python3
"""
Export trained KWS word model checkpoint to C header for ESP runtime.

Reads:
  - pytorch checkpoint from train_kws.py (kws_best.pt)
  - optional word->intent json map

Writes:
  - components/services_mic/include/mic_kws_word_model.h
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Dict, List, Tuple

import torch


def fold_bn(
    conv_w: torch.Tensor,
    bn_w: torch.Tensor,
    bn_b: torch.Tensor,
    bn_mean: torch.Tensor,
    bn_var: torch.Tensor,
    eps: float = 1e-5,
) -> Tuple[torch.Tensor, torch.Tensor]:
    # Conv has no bias in train model. Fold BN into conv+bias.
    scale = bn_w / torch.sqrt(bn_var + eps)
    view_shape = [conv_w.shape[0]] + [1] * (conv_w.ndim - 1)
    w = conv_w * scale.view(*view_shape)
    b = bn_b - bn_mean * scale
    return w, b


def intent_to_enum(intent: str) -> str:
    lookup = {
        "love": "ORB_INTENT_LOVE",
        "future": "ORB_INTENT_FUTURE",
        "choice": "ORB_INTENT_CHOICE",
        "money": "ORB_INTENT_MONEY",
        "path": "ORB_INTENT_PATH",
        "luck": "ORB_INTENT_PATH",
        "danger": "ORB_INTENT_DANGER",
        "inner_state": "ORB_INTENT_INNER_STATE",
        "wish": "ORB_INTENT_WISH",
        "yes_no": "ORB_INTENT_YES_NO",
    }
    return lookup.get(intent, "ORB_INTENT_UNKNOWN")


def format_floats(name: str, t: torch.Tensor) -> List[str]:
    flat = t.contiguous().view(-1).tolist()
    lines = [f"static const float {name}[{len(flat)}] = {{"]
    row: List[str] = []
    for i, v in enumerate(flat, 1):
        row.append(f"{float(v):.9e}f")
        if len(row) >= 8:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines.append("};")
    return lines


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", type=Path, required=True)
    ap.add_argument("--word-intent-map", type=Path, required=True)
    ap.add_argument("--out-header", type=Path, required=True)
    ap.add_argument("--min-conf-permille", type=int, default=560)
    args = ap.parse_args()

    ckpt = torch.load(args.checkpoint, map_location="cpu")
    sd: Dict[str, torch.Tensor] = ckpt["model_state"]
    labels: List[str] = list(ckpt["labels"])
    width: int = int(ckpt["width"])
    n_mels: int = int(ckpt.get("n_mels", 40))
    sample_rate: int = int(ckpt.get("sample_rate", 16000))

    if n_mels != 40 or sample_rate != 16000:
        raise RuntimeError(f"unexpected frontend params: n_mels={n_mels}, sample_rate={sample_rate}")

    with args.word_intent_map.open("r", encoding="utf-8") as f:
        word_to_intent = json.load(f)

    stem_w, stem_b = fold_bn(
        sd["stem.0.weight"],
        sd["stem.1.weight"],
        sd["stem.1.bias"],
        sd["stem.1.running_mean"],
        sd["stem.1.running_var"],
    )

    dw_w: List[torch.Tensor] = []
    dw_b: List[torch.Tensor] = []
    pw_w: List[torch.Tensor] = []
    pw_b: List[torch.Tensor] = []
    for i in range(4):
        w, b = fold_bn(
            sd[f"blocks.{i}.depthwise.weight"],
            sd[f"blocks.{i}.dw_bn.weight"],
            sd[f"blocks.{i}.dw_bn.bias"],
            sd[f"blocks.{i}.dw_bn.running_mean"],
            sd[f"blocks.{i}.dw_bn.running_var"],
        )
        dw_w.append(w)
        dw_b.append(b)

        w, b = fold_bn(
            sd[f"blocks.{i}.pointwise.weight"],
            sd[f"blocks.{i}.pw_bn.weight"],
            sd[f"blocks.{i}.pw_bn.bias"],
            sd[f"blocks.{i}.pw_bn.running_mean"],
            sd[f"blocks.{i}.pw_bn.running_var"],
        )
        pw_w.append(w)
        pw_b.append(b)

    head_w = sd["head.weight"]
    head_b = sd["head.bias"]

    mel_fb = sd["front.mel.mel_scale.fb"]  # [257, 40]
    win = sd["front.mel.spectrogram.window"]  # [400]

    class_to_intent: List[str] = []
    class_names_ascii: List[str] = []
    for idx, label in enumerate(labels):
        if label == "silence":
            class_to_intent.append("ORB_INTENT_UNKNOWN")
            class_names_ascii.append("silence")
            continue
        intent = word_to_intent.get(label, None)
        class_to_intent.append(intent_to_enum(intent if intent is not None else ""))
        # Keep safe ASCII ids for logs/debug.
        class_names_ascii.append(f"class_{idx}")

    out: List[str] = []
    out.append("#ifndef MIC_KWS_WORD_MODEL_H")
    out.append("#define MIC_KWS_WORD_MODEL_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append('#include "app_defs.h"')
    out.append("")
    out.append("#define MIC_KWS_SAMPLE_RATE 16000")
    out.append("#define MIC_KWS_WINDOW_SAMPLES 16000")
    out.append("#define MIC_KWS_NFFT 512")
    out.append("#define MIC_KWS_WIN_LENGTH 400")
    out.append("#define MIC_KWS_HOP_LENGTH 160")
    out.append("#define MIC_KWS_PAD 256")
    out.append("#define MIC_KWS_N_MELS 40")
    out.append("#define MIC_KWS_FRAMES 101")
    out.append(f"#define MIC_KWS_WIDTH {width}")
    out.append(f"#define MIC_KWS_CLASS_COUNT {len(labels)}")
    out.append(f"#define MIC_KWS_MIN_CONF_PERMILLE {int(args.min_conf_permille)}U")
    out.append("")

    out.extend(format_floats("s_mic_kws_hann_window", win))
    out.append("")
    out.extend(format_floats("s_mic_kws_mel_fb", mel_fb))
    out.append("")
    out.extend(format_floats("s_mic_kws_stem_w", stem_w))
    out.append("")
    out.extend(format_floats("s_mic_kws_stem_b", stem_b))
    out.append("")

    for i in range(4):
        out.extend(format_floats(f"s_mic_kws_dw{i}_w", dw_w[i]))
        out.append("")
        out.extend(format_floats(f"s_mic_kws_dw{i}_b", dw_b[i]))
        out.append("")
        out.extend(format_floats(f"s_mic_kws_pw{i}_w", pw_w[i]))
        out.append("")
        out.extend(format_floats(f"s_mic_kws_pw{i}_b", pw_b[i]))
        out.append("")

    out.extend(format_floats("s_mic_kws_head_w", head_w))
    out.append("")
    out.extend(format_floats("s_mic_kws_head_b", head_b))
    out.append("")

    out.append("static const orb_intent_id_t s_mic_kws_class_to_intent[MIC_KWS_CLASS_COUNT] = {")
    for v in class_to_intent:
        out.append(f"    {v},")
    out.append("};")
    out.append("")

    out.append("static const char *s_mic_kws_class_name[MIC_KWS_CLASS_COUNT] = {")
    for s in class_names_ascii:
        out.append(f'    "{s}",')
    out.append("};")
    out.append("")
    out.append("#endif")
    out.append("")

    args.out_header.parent.mkdir(parents=True, exist_ok=True)
    args.out_header.write_text("\n".join(out), encoding="utf-8")
    print(f"exported: {args.out_header}")
    print(f"labels: {len(labels)} width: {width}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
