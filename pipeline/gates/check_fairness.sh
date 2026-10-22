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
out = pathlib.Path(ctx["artifacts_dir"]) / "gates"
out.mkdir(parents=True, exist_ok=True)

fairness = ctx.get("fairness", {})
kernel_templates = fairness.get("kernel_search_paths", [])
kernel_candidates = [root / tpl.replace("{N}", query) for tpl in kernel_templates]

issues = []
checked_paths: list[str] = []
existing = [path for path in kernel_candidates if path.is_file()]
if not existing:
    issues.append("kernel file missing")
else:
    for kernel in existing:
        rel = str(kernel.relative_to(root))
        checked_paths.append(rel)
        text = kernel.read_text()
        if re.search(r"\bgetenv\s*\(", text):
            issues.append(f"getenv in kernel ({rel})")
        for constant in fairness.get("cardinality_constants", []):
            const = str(constant)
            # Whole-token match only — avoid flagging 6000001 for constant 600000.
            if re.search(rf"(?<![0-9]){re.escape(const)}(?![0-9])", text):
                issues.append(f"possible cardinality constant: {const} ({rel})")

result = {
    "gate": "fairness",
    "query": int(query),
    "status": "pass" if not issues else "fail",
    "kernel_path": checked_paths[0] if checked_paths else None,
    "checked_paths": checked_paths,
    "issues": issues,
}
path = out / "fairness.json"
path.write_text(json.dumps(result, indent=2))
print(path.read_text())
PY
