#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import random
import sys
from pathlib import Path
from typing import Dict, List


def _bootstrap_path() -> None:
    kws_root = Path(__file__).resolve().parents[1]
    kws_root_str = str(kws_root)
    if kws_root_str not in sys.path:
        sys.path.insert(0, kws_root_str)


SAMPLES: Dict[str, List[str]] = {
    "love": [
        "Когда я встречу любовь?",
        "Будут ли у нас отношения?",
    ],
    "money": [
        "Когда у меня наладятся деньги?",
        "Стоит ли менять работу ради дохода?",
    ],
    "future": [
        "Что меня ждет в ближайшем будущем?",
        "Какие перемены скоро придут?",
    ],
    "choice": [
        "Что мне выбрать сейчас?",
        "Стоит ли действовать или подождать?",
    ],
    "danger": [
        "Есть ли рядом опасность?",
        "Стоит ли доверять этому человеку?",
    ],
    "path": [
        "Какой путь мне выбрать?",
        "Стоит ли ехать в эту поездку?",
    ],
    "inner_state": [
        "Почему мне тревожно?",
        "Как мне обрести внутренний покой?",
    ],
    "wish": [
        "Сбудется ли мое желание?",
        "Что мешает мечте исполниться?",
    ],
    "time": [
        "Когда наступит удачный период?",
        "Какая дата будет благоприятной?",
    ],
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Smoke-test local oracle LLM script generation")
    p.add_argument("--count", type=int, default=3, help="How many random generations to run")
    p.add_argument("--seed", type=int, default=-1, help="Random seed; -1 means random")
    p.add_argument("--intent", default="", help="Optional fixed intent instead of random")
    p.add_argument("--question", default="", help="Optional fixed question text")
    p.add_argument("--form", default="open", choices=["open", "yes_no"])
    p.add_argument("--polarity", default="neutral", choices=["neutral", "positive", "negative"])
    p.add_argument("--confidence", type=float, default=0.7)

    p.add_argument("--endpoint", default="http://127.0.0.1:8080/v1/chat/completions")
    p.add_argument("--model", default="qwen2.5-1.5b-instruct-q4_k_m")
    p.add_argument("--timeout-ms", type=int, default=30000)
    p.add_argument("--temperature", type=float, default=0.2)
    p.add_argument("--max-output-tokens", type=int, default=96)
    p.add_argument("--api-key", default="")
    return p.parse_args()


async def main_async(args: argparse.Namespace) -> int:
    _bootstrap_path()
    from pi_ws.oracle_llm import OracleLlmConfig, OracleLlmInput, build_oracle_llm  # noqa: WPS433

    rng = random.Random(None if args.seed < 0 else int(args.seed))

    llm, probe = build_oracle_llm(
        OracleLlmConfig(
            backend="local",
            timeout_ms=int(args.timeout_ms),
            temperature=float(args.temperature),
            max_output_tokens=int(args.max_output_tokens),
            local_endpoint=str(args.endpoint).strip(),
            local_model=str(args.model).strip(),
            local_api_key=str(args.api_key).strip(),
        )
    )
    if llm is None:
        print(f"LLM disabled: {probe}")
        return 2

    print(f"oracle_llm probe: {probe}")

    intents = list(SAMPLES.keys())
    count = max(1, int(args.count))
    fixed_intent = args.intent.strip()
    fixed_question = args.question.strip()

    for idx in range(count):
        intent = fixed_intent if fixed_intent else rng.choice(intents)
        if intent not in SAMPLES:
            print(f"\n[{idx + 1}/{count}] skip unknown intent={intent!r}")
            continue
        question = fixed_question if fixed_question else rng.choice(SAMPLES[intent])
        result = await llm.generate(
            OracleLlmInput(
                text=question,
                intent=intent,
                question_form=args.form,
                polarity=args.polarity,
                confidence=float(args.confidence),
            )
        )

        print(f"\n[{idx + 1}/{count}] intent={intent} form={args.form} polarity={args.polarity}")
        print(f"Q: {question}")
        if result is None:
            print("LLM result: <none> (fallback would be used in server)")
            continue
        print(f"provider: {result.provider}")
        print(f"greeting: {result.greeting}")
        print(f"understanding: {result.understanding}")
        print(f"prediction: {result.prediction}")
        print(f"farewell: {result.farewell}")
        print("joined:")
        print(result.text)

    return 0


def main() -> int:
    return asyncio.run(main_async(parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())
