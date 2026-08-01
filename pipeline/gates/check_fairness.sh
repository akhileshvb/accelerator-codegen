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

sys.path.insert(0, str(root / "pipeline" / "gates"))
from read_gate_context import find_gate_context

ctx = find_gate_context(root, query)
art_ns = ctx.get("artifact_namespace", "tpch")
out = root / "pipeline" / "artifacts" / art_ns / f"q{query}" / "gates"
out.mkdir(parents=True, exist_ok=True)

fairness = ctx.get("fairness", {})
kernel_templates = fairness.get(
    "kernel_search_paths",
    [
        f"src/generated/tpch/single-threaded/q{query}_kernel.cpp",
        f"src/generated/tpch/q{query}_kernel.cpp",
    ],
)
kernel_candidates = [root / tpl.replace("{N}", query) for tpl in kernel_templates]

issues = []
kernel_path = None
kernel = next((path for path in kernel_candidates if path.is_file()), None)
if kernel is not None:
    kernel_path = str(kernel.relative_to(root))
    text = kernel.read_text()
    if re.search(r"\bgetenv\s*\(", text):
        issues.append("getenv in kernel")
    for constant in fairness.get("cardinality_constants", []):
        if constant in text:
            issues.append(f"possible cardinality constant: {constant}")
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
