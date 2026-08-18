#!/usr/bin/env python3
"""Compare optimization worker metrics artifact shape (legacy vs worker)."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize optimization_worker_metrics.json")
    parser.add_argument("artifacts_dir", type=Path, help="Query artifacts directory")
    args = parser.parse_args()
    path = args.artifacts_dir / "optimization_worker_metrics.json"
    if not path.is_file():
        print(f"no metrics at {path}")
        return 1
    data = json.loads(path.read_text(encoding="utf-8"))
    print(json.dumps(data, indent=2))
    inv = data.get("runner_invocations", 0)
    tools = data.get("episode_tool_calls", 0)
    if inv:
        print(f"tool_calls_per_runner: {tools / inv:.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
