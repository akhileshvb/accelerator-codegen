#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_bench_sweep.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
ART="${GATE_ART}"
mkdir -p "${OUT}"

write_skip() {
  local reason="$1"
  python3 - <<PY
import json, pathlib
out = pathlib.Path("${OUT}/aux_bench_sweep.json")
out.write_text(json.dumps({
    "gate": "aux_bench_sweep",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "engine_name": "${ENGINE_NAME}",
    "status": "skip",
    "reason": """${reason}""",
}, indent=2))
print(out.read_text())
PY
}

BENCH_TARGET="${AUX_BENCH_TARGET}"
if [[ -z "${BENCH_TARGET}" ]]; then
  write_skip "no aux bench target in the gate context"
  exit 0
fi
if ! engine_ready "${BENCH_TARGET}"; then
  emit_result "${OUT}/aux_bench_sweep.json" aux_bench_sweep fail "${ENGINE_FAIL}" "${ENGINE_LOG}" "query:=${QUERY}"
fi

export ACCEL_AUX_BENCH_WORK_DIR="${BUILD}/${AUX_BENCH_WORK_DIR_SUFFIX:-q${QUERY}_aux_bench_sf1}"
# Prefer gate-context write frequencies. Do not inherit ambient optimization
# quick-sweep env (e.g. ACCEL_AUX_WRITE_FREQUENCIES=0,10) into fair audit/impl sweeps.
if [[ -n "${AUX_WRITE_FREQUENCIES_CTX:-}" ]]; then
  export ACCEL_AUX_WRITE_FREQUENCIES="${AUX_WRITE_FREQUENCIES_CTX}"
else
  export ACCEL_AUX_WRITE_FREQUENCIES="${ACCEL_AUX_WRITE_FREQUENCIES:-0,1,10,50}"
fi
export ACCEL_AUX_BENCH_SUITES="${ACCEL_AUX_BENCH_SUITES:-${AUX_BENCH_SUITES}}"
export ACCEL_AUX_BENCHMARK="${BENCHMARK_NAME}"
export ACCEL_AUX_BENCH_TARGET="${BENCH_TARGET}"

CHANGE_KIND="${ACCEL_GATE_CHANGE_KIND:-}"
if [[ "${CHANGE_KIND}" == "optimization" ]]; then
  OPT_SWEEPS="$(python3 -c "import json,sys; print(json.dumps(json.loads(sys.argv[1]).get('aux_bench_sweep_optimization') or {}))" "${CTX}")"
  export ACCEL_BENCH_WARMUP="${ACCEL_BENCH_WARMUP:-$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('bench_warmup', 5))" "${OPT_SWEEPS}")}"
  export ACCEL_BENCH_QUERIES="${ACCEL_BENCH_QUERIES:-$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('bench_queries', 25))" "${OPT_SWEEPS}")}"
  export ACCEL_BENCH_REF_SAMPLES="${ACCEL_BENCH_REF_SAMPLES:-$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('bench_ref_samples', 3))" "${OPT_SWEEPS}")}"
  export ACCEL_BENCH_BASE_WRITE_SAMPLES="${ACCEL_BENCH_BASE_WRITE_SAMPLES:-$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('bench_base_write_samples', 2))" "${OPT_SWEEPS}")}"
else
  IMPL_SWEEPS="$(python3 -c "import json,sys; print(json.dumps(json.loads(sys.argv[1]).get('aux_bench_sweep') or {}))" "${CTX}")"
  # Context wins over ambient env left over from optimization quick benches.
  export ACCEL_BENCH_WARMUP="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('bench_warmup', 10))" "${IMPL_SWEEPS}")"
  export ACCEL_BENCH_QUERIES="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('bench_queries', 100))" "${IMPL_SWEEPS}")"
  export ACCEL_BENCH_REF_SAMPLES="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('bench_ref_samples', 3))" "${IMPL_SWEEPS}")"
  export ACCEL_BENCH_BASE_WRITE_SAMPLES="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('bench_base_write_samples', 3))" "${IMPL_SWEEPS}")"
  if python3 -c "import json,sys; print('1' if json.loads(sys.argv[1]).get('skip_duckdb_ref') else '0')" "${IMPL_SWEEPS}" | grep -q 1; then
    export ACCEL_BENCH_SKIP_DUCKDB_REF=1
  else
    unset ACCEL_BENCH_SKIP_DUCKDB_REF
  fi
  scan_limit="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('bench_scan_limit') or '')" "${IMPL_SWEEPS}")"
  if [[ -n "${scan_limit}" ]]; then
    export ACCEL_Q21_BENCH_SCAN_LIMIT="${scan_limit}"
  else
    unset ACCEL_Q21_BENCH_SCAN_LIMIT
  fi
fi
export ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB="${ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB:-1}"
export ACCEL_REPO_ROOT="${ACCEL_REPO_ROOT:-${ROOT}}"
export ACCEL_AUX_BENCH_BASELINE_CACHE="${ACCEL_AUX_BENCH_BASELINE_CACHE:-${ROOT}/accelerators/${BENCHMARK_NAME}/q${QUERY}/config/q${QUERY}_aux_benchmark_baseline.json}"

EXPECTED_FREQS="${ACCEL_AUX_WRITE_FREQUENCIES}"

SWEEP_LOG="${OUT}/aux_bench_sweep.log"
RESULTS_CSV="${ACCEL_AUX_BENCH_WORK_DIR}/results.csv"
mkdir -p "${ACCEL_AUX_BENCH_WORK_DIR}"
: > "${RESULTS_CSV}"
: > "${SWEEP_LOG}"
SWEEP_RC=0
IFS=',' read -r -a SUITE_LIST <<< "${ACCEL_AUX_BENCH_SUITES}"
IFS=',' read -r -a FREQ_LIST <<< "${EXPECTED_FREQS}"
unset "ACCEL_Q${QUERY}_WRITE_FREQUENCY_PCT"   # the point is the CLI argument (query_host_bootstrap.md)
for suite in "${SUITE_LIST[@]}"; do
  export "ACCEL_Q${QUERY}_BENCH_SUITE=${suite}"
  for pct in "${FREQ_LIST[@]}"; do
    engine_measure "${BENCH_TARGET}" --query "${QUERY}" "${pct// /}"
    printf '%s\n' "${LOG}" >> "${SWEEP_LOG}"
    [[ ${RC} -eq 0 ]] || SWEEP_RC=${RC}
    # keep the CSV rows (one header), drop banners
    printf '%s\n' "${LOG}" | awk -F, -v hdr="$([[ -s "${RESULTS_CSV}" ]] && echo 1 || echo 0)" '
      /^suite,/ { if (!hdr) { print; hdr = 1 }; next }
      /^speedup_gate/ || /^#/ { next }
      NF >= 10 { print }' >> "${RESULTS_CSV}"
  done
done

python3 - <<PY
import json
import os
import pathlib
import sys

sys.path.insert(0, "${ROOT}/pipeline/orchestrator")
from accel_pipeline.aux_bench import record_aux_bench_sweep_outcome, summarize_sweep_csv
from accel_pipeline.config import load_workload

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
    "benchmark": "${BENCHMARK_NAME}",
    "status": status,
    "composite_score": summary.get("composite_score", 0.0),
    "composite_workload_speedup": summary.get("composite_workload_speedup", 0.0),
    "read_only_query_speedup": summary.get("read_only_query_speedup", 0.0),
    "read_only_cold_start_workload_speedup": summary.get("read_only_cold_start_workload_speedup", 0.0),
    "by_write_frequency_pct": summary.get("by_write_frequency_pct", {}),
    "results_csv": str(results_csv),
    "notes": "; ".join(notes) if notes else "sweep complete",
    "log_tail": log_tail,
}
out = pathlib.Path("${OUT}/aux_bench_sweep.json")
out.write_text(json.dumps(gate, indent=2))
summary_path = pathlib.Path("${ART}/aux_bench_summary.json")
summary_path.write_text(json.dumps(summary, indent=2))

if status == "pass":
    bench = "${BENCHMARK_NAME}"
    query = int("${QUERY}")
    workload_name = f"{bench}_q{query}_aux"
    try:
        workload = load_workload(workload_name, pathlib.Path("${ROOT}"))
    except FileNotFoundError:
        workload = None
    change_kind = os.environ.get("ACCEL_GATE_CHANGE_KIND", "bench_sweep")
    iteration = int(os.environ.get("ACCEL_GATE_ITERATION", "0"))
    record_aux_bench_sweep_outcome(
        pathlib.Path("${ROOT}"),
        query,
        summary,
        iteration=iteration,
        change_kind=change_kind,
        workload=workload,
    )

print(out.read_text())
sys.exit(0 if status == "pass" else 1)
PY
