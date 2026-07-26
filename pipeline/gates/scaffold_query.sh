#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: scaffold_query.sh <query_number>" >&2
  exit 1
fi

python3 - "${ROOT}" "${QUERY}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
query = sys.argv[2]
pipeline = root / "pipeline"
out = pipeline / "artifacts" / f"q{query}" / "scaffold"
out.mkdir(parents=True, exist_ok=True)

reg_tpl = (pipeline / "templates/scaffold/register_snippet.cpp.tpl").read_text()
cmake_tpl = (pipeline / "templates/scaffold/cmake_snippet.cmake.tpl").read_text()

subs = {
    "query_id": query,
    "create_fn": f"CreateTpchQ{query}FunctionSet",
    "function_name": f"accel_tpch_q{query}",
}
for k, v in subs.items():
    reg_tpl = reg_tpl.replace("{{" + k + "}}", v)
    cmake_tpl = cmake_tpl.replace("{{" + k + "}}", v)

(out / "register_snippet.cpp").write_text(reg_tpl)
(out / "cmake_snippet.cmake").write_text(cmake_tpl)
# TODO: Idempotently patch src/register.cpp and CMakeLists.txt (marker-based insert).
# TODO: Emit tests/test_support/q{N}_manifest.hpp from seed manifest scan.
print(json.dumps({"gate": "scaffold", "query": int(query), "status": "pass", "out": str(out)}))
PY
