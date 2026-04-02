#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path


def _bootstrap_path() -> None:
    # Allow running from repo root on dev machine:
    # python kws/scripts/pi_mic_ws_server.py ...
    kws_root = Path(__file__).resolve().parents[1]
    kws_root_str = str(kws_root)
    if kws_root_str not in sys.path:
        sys.path.insert(0, kws_root_str)


def main() -> int:
    _bootstrap_path()
    from pi_ws.server import main as server_main

    return server_main()


if __name__ == "__main__":
    raise SystemExit(main())

