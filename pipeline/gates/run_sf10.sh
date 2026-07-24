#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ROOT}/build-release"
DB="${ROOT}/../accelerator-codegen-duckdb/tpch_sf10.duckdb"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_sf10.sh <query_number>" >&2
  exit 1
fi

OUT="${ROOT}/pipeline/artifacts/q${QUERY}/gates"
mkdir -p "${OUT}"
BIN="${BUILD}/benchmark_tpch_q${QUERY}"

if [[ ! -x "${BIN}" ]] || [[ ! -f "${DB}" ]]; then
  python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/sf10.json").write_text(json.dumps({
    "gate": "sf10", "query": int("${QUERY}"), "status": "skip",
    "reason": "benchmark binary or sf10 db missing",
}, indent=2))
PY
  exit 0
fi

set +e
LOG="$(env ACCEL_TPCH_DB="${DB}" "${BIN}" 2>&1)"
RC=$?
set -e

STATUS="pass" if [[ ${RC} -eq 0 ]] else "fail"
python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/sf10.json").write_text(json.dumps({
    "gate": "sf10",
    "query": int("${QUERY}"),
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
PY
