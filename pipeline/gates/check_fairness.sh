#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: check_fairness.sh <query_number>" >&2
  exit 1
fi

python3 - "${ROOT}" "${QUERY}" <<'PY'
import json
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
query = sys.argv[2]
out = root / "pipeline" / "artifacts" / f"q{query}" / "gates"
out.mkdir(parents=True, exist_ok=True)

kernel_candidates = [
    root / "src" / "generated" / "tpch" / f"q{query}_kernel.cpp",
    root / "src" / "generated" / "tpch" / "single-threaded" / f"q{query}_kernel.cpp",
    root / "src" / "generated" / "tpch" / "single-threaded" / f"q{query}_fused_scan.cpp",
]
kernel = next((path for path in kernel_candidates if path.is_file()), kernel_candidates[0])
issues = []
kernel_path = None
if kernel.is_file():
    kernel_path = str(kernel.relative_to(root))
    text = kernel.read_text()
    if re.search(r"\bgetenv\s*\(", text):
        issues.append("getenv in kernel")
    if re.search(r"\b(150000|600000|800000)\b", text):
        issues.append("possible SF1 cardinality constant")
else:
    issues.append("kernel file missing")

result = {
    "gate": "fairness",
    "query": int(query),
    "status": "pass" if not issues else "fail",
    "kernel_path": kernel_path,
    "issues": issues,
}
path = out / "fairness.json"
path.write_text(json.dumps(result, indent=2))
print(path.read_text())
PY
