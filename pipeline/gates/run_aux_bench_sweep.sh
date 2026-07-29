#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ROOT}/build-release"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_bench_sweep.sh <query_number>" >&2
  exit 1
fi

OUT="${ROOT}/pipeline/artifacts/q${QUERY}/gates"
ART="${ROOT}/pipeline/artifacts/q${QUERY}"
mkdir -p "${OUT}"

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

write_skip() {
  local reason="$1"
  python3 - <<PY
import json, pathlib
out = pathlib.Path("${OUT}/aux_bench_sweep.json")
out.write_text(json.dumps({
    "gate": "aux_bench_sweep",
    "query": int("${QUERY}"),
    "status": "skip",
    "reason": """${reason}""",
}, indent=2))
print(out.read_text())
PY
}

if [[ ! -d "${BUILD}" ]]; then
  write_skip "build dir missing"
  exit 0
fi

BENCH_TARGET="benchmark_tpch_q${QUERY}_aux"
BUILD_LOG=""
if ! ensure_cmake_target "${ROOT}" "${BUILD}" "${BENCH_TARGET}"; then
	BUILD_LOG="$(cmake --build "${BUILD}" --target "${BENCH_TARGET}" -j"$(nproc)" 2>&1 | tail -40 || true)"
fi
if [[ ! -x "${BUILD}/${BENCH_TARGET}" ]]; then
	write_skip "benchmark binary missing after cmake configure/build"
	exit 0
fi

export ACCEL_BUILD_DIR="${BUILD}"
export ACCEL_AUX_BENCH_WORK_DIR="${BUILD}/q${QUERY}_aux_bench_sf1"
export ACCEL_AUX_WRITE_FREQUENCIES="${ACCEL_AUX_WRITE_FREQUENCIES:-0,1,10,50}"
export ACCEL_BENCH_WARMUP="${ACCEL_BENCH_WARMUP:-10}"
export ACCEL_BENCH_QUERIES="${ACCEL_BENCH_QUERIES:-100}"
export ACCEL_BENCH_REF_SAMPLES="${ACCEL_BENCH_REF_SAMPLES:-5}"
export ACCEL_BENCH_BASE_WRITE_SAMPLES="${ACCEL_BENCH_BASE_WRITE_SAMPLES:-3}"
export ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB="${ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB:-1}"

EXPECTED_FREQS="${ACCEL_AUX_WRITE_FREQUENCIES}"

SWEEP_LOG="${OUT}/aux_bench_sweep.log"
set +e
"${ROOT}/scripts/run_aux_bench_sweep.sh" "${QUERY}" > "${SWEEP_LOG}" 2>&1
SWEEP_RC=$?
set -e

RESULTS_CSV="${ACCEL_AUX_BENCH_WORK_DIR}/results.csv"

python3 - <<PY
import json
import pathlib
import sys

sys.path.insert(0, "${ROOT}/pipeline/orchestrator")
from accel_pipeline.aux_bench import summarize_sweep_csv

results_csv = pathlib.Path("${RESULTS_CSV}")
log_path = pathlib.Path("${SWEEP_LOG}")
log_tail = log_path.read_text(encoding="utf-8")[-3000:] if log_path.is_file() else ""

summary = summarize_sweep_csv(results_csv) if results_csv.is_file() else {
    "status": "missing_csv",
    "row_count": 0,
    "composite_score": 0.0,
}

status = "pass"
notes = []
expected_rows = len([f for f in "${EXPECTED_FREQS}".split(",") if f.strip()])
if int("${SWEEP_RC}") != 0:
    status = "fail"
    notes.append("bench sweep script failed")
if summary.get("status") in ("empty", "missing_csv"):
    status = "fail"
    notes.append(f"sweep status={summary.get('status')}")
elif summary.get("row_count", 0) < expected_rows:
    status = "fail"
    notes.append(
        f"expected {expected_rows} sweep rows, got {summary.get('row_count', 0)} "
        "(benchmark must emit CSV with workload_speedup per write frequency)"
    )

gate = {
    "gate": "aux_bench_sweep",
    "query": int("${QUERY}"),
    "status": status,
    "composite_score": summary.get("composite_score", 0.0),
    "composite_workload_speedup": summary.get("composite_workload_speedup", 0.0),
    "read_only_query_speedup": summary.get("read_only_query_speedup", 0.0),
    "by_write_frequency_pct": summary.get("by_write_frequency_pct", {}),
    "results_csv": str(results_csv),
    "notes": "; ".join(notes) if notes else "sweep complete",
    "log_tail": log_tail,
}
out = pathlib.Path("${OUT}/aux_bench_sweep.json")
out.write_text(json.dumps(gate, indent=2))
summary_path = pathlib.Path("${ART}/aux_bench_summary.json")
summary_path.write_text(json.dumps(summary, indent=2))
print(out.read_text())
sys.exit(0 if status == "pass" else 1)
PY
