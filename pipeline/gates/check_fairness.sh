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

kernel = root / "src" / "generated" / "tpch" / f"q{query}_kernel.cpp"
if not kernel.is_file():
    alt = root / "src" / "generated" / "tpch" / "single-threaded" / f"q{query}_fused_scan.cpp"
    if alt.is_file():
        kernel = alt
issues = []
if kernel.is_file():
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
    "issues": issues,
}
path = out / "fairness.json"
path.write_text(json.dumps(result, indent=2))
print(path.read_text())
PY
