#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: scaffold_query.sh <query_number>" >&2
  exit 1
fi

CTX="$(python3 "$(dirname "${BASH_SOURCE[0]}")/read_gate_context.py" "${ROOT}" "${QUERY}")"
ART_DIR="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifacts_dir'])" "${CTX}")"
CREATE_FN="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['create_fn'])" "${CTX}")"
FUNCTION_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['function_name'])" "${CTX}")"
SOURCE_SUBDIR="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['source_subdir'])" "${CTX}")"
TEST_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['test_target'])" "${CTX}")"
BENCH_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['bench_target'])" "${CTX}")"
BENCHMARK_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['benchmark_name'])" "${CTX}")"
ENGINE_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['engine_name'])" "${CTX}")"

python3 - "${ROOT}" "${QUERY}" "${ART_DIR}" "${CREATE_FN}" "${FUNCTION_NAME}" "${SOURCE_SUBDIR}" "${TEST_TARGET}" "${BENCH_TARGET}" "${BENCHMARK_NAME}" "${ENGINE_NAME}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
query = sys.argv[2]
art_dir = pathlib.Path(sys.argv[3])
create_fn = sys.argv[4]
function_name = sys.argv[5]
source_subdir = sys.argv[6]
test_target = sys.argv[7]
bench_target = sys.argv[8]
benchmark_name = sys.argv[9]
engine_name = sys.argv[10]

pipeline = root / "pipeline"
out = art_dir / "scaffold"
out.mkdir(parents=True, exist_ok=True)

cmake_tpl = (pipeline / "templates/scaffold/cmake_snippet.cmake.tpl").read_text()

subs = {
    "query_id": query,
    "create_fn": create_fn,
    "function_name": function_name,
    "source_subdir": source_subdir,
    "test_target": test_target,
    "bench_target": bench_target,
    "benchmark_name": benchmark_name,
    "engine_name": engine_name,
}
for k, v in subs.items():
    cmake_tpl = cmake_tpl.replace("{{" + k + "}}", v)

(out / "cmake_snippet.cmake").write_text(cmake_tpl)
print(json.dumps({"gate": "scaffold", "query": int(query), "status": "pass", "out": str(out), "engine_name": engine_name}))
PY


python3 - "${ROOT}" "${QUERY}" "${ENGINE_NAME}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
query = int(sys.argv[2])
engine = sys.argv[3]
sys.path.insert(0, str(root / "pipeline" / "orchestrator"))

from accel_pipeline.scaffold_stubs import materialize_from_gate

result = materialize_from_gate(root, query, engine)
print(json.dumps({"gate": "scaffold_stubs", "query": query, **result}))
PY
