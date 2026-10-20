#!/usr/bin/env python3
"""Print the gate context JSON the orchestrator wrote for this gate run (ACCEL_GATE_CONTEXT)."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def find_gate_context(repo_root: Path, query: str) -> dict:
    path = os.environ.get("ACCEL_GATE_CONTEXT")
    if not path:
        raise SystemExit("ACCEL_GATE_CONTEXT is not set: gates run through the orchestrator's run_gate")
    return json.loads(Path(path).read_text(encoding="utf-8"))


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: read_gate_context.py <repo_root> <query>", file=sys.stderr)
        return 1
    print(json.dumps(find_gate_context(Path(sys.argv[1]), sys.argv[2])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
