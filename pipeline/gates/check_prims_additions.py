#!/usr/bin/env python3
"""Backstop for the primitives library: relative to git HEAD it may only grow.

primitive_publish enforces additions-only at publish time; this gate catches an
edit that bypassed it (e.g. a backend with native file access).  Prints JSON,
exit 1 on any removed or changed line in include/accelerator_codegen/stateful/prims/
or tests/aux_state/test_prims.cpp.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

PATHS = ["include/accelerator_codegen/stateful/prims", "tests/aux_state/test_prims.cpp"]


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    proc = subprocess.run(["git", "diff", "-U0", "HEAD", "--", *PATHS], cwd=root, capture_output=True, text=True)
    if proc.returncode != 0:
        print(json.dumps({"status": "skip", "reason": "git diff failed: " + proc.stderr.strip()[:200]}))
        return 0
    removed, current = [], ""
    for line in proc.stdout.splitlines():
        if line.startswith("+++ "):
            current = line[6:]
        elif line.startswith("-") and not line.startswith("---"):
            removed.append(f"{current}: {line[:160]}")
    print(json.dumps({"status": "fail" if removed else "pass", "removed_or_changed_lines": removed[:40]}, indent=2))
    return 1 if removed else 0


if __name__ == "__main__":
    sys.exit(main())
