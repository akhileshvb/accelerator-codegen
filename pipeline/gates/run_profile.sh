#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ROOT}/build-release"
DB="${ROOT}/../accelerator-codegen-duckdb/tpch_sf1.duckdb"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_profile.sh <query_number>" >&2
  exit 1
fi

OUT="${ROOT}/pipeline/artifacts/q${QUERY}/gates"
mkdir -p "${OUT}"
BIN="${BUILD}/benchmark_tpch_q${QUERY}"

if [[ ! -x "${BIN}" ]]; then
  python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/profile.json").write_text(json.dumps({
    "gate": "profile", "query": int("${QUERY}"), "status": "skip",
    "reason": "benchmark binary missing",
}, indent=2))
PY
  exit 0
fi

set +e
LOG="$(env ACCEL_TPCH_DB="${DB}" ACCEL_BENCH_PROFILE=1 "${BIN}" 2>&1)"
RC=$?
set -e

STATUS="pass" if [[ ${RC} -eq 0 ]] else "fail"
python3 - <<PY
import json, pathlib
profile_path = pathlib.Path("${ROOT}/pipeline/artifacts/q${QUERY}/profile_summary.json")
profile_path.write_text(json.dumps({"log_tail": """${LOG}"""[-8000:]}, indent=2))
pathlib.Path("${OUT}/profile.json").write_text(json.dumps({
    "gate": "profile",
    "query": int("${QUERY}"),
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "profile_summary": str(profile_path),
}, indent=2))
PY
