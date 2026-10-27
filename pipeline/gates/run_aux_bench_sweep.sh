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
source "$(dirname "${BASH_SOURCE[0]}")/suite_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
ART="${GATE_ART}"
mkdir -p "${OUT}"

write_skip() {
  local reason="$1"
  REASON_TEXT="${reason}" python3 - <<PY
import os, json, pathlib
out = pathlib.Path("${OUT}/aux_bench_sweep.json")
out.write_text(json.dumps({
    "gate": "aux_bench_sweep",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "engine_name": "${ENGINE_NAME}",
    "status": "skip",
    "reason": os.environ["REASON_TEXT"],
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

export ACCEL_AUX_BENCH_WORK_DIR="${BUILD}/${AUX_BENCH_WORK_DIR_SUFFIX:-q${QUERY}_aux_bench}"
# Prefer gate-context write frequencies. Do not inherit ambient optimization
# quick-sweep env (e.g. ACCEL_AUX_WRITE_FREQUENCIES=0,10) into fair audit/impl sweeps.
if [[ -n "${AUX_WRITE_FREQUENCIES_CTX:-}" ]]; then
  export ACCEL_AUX_WRITE_FREQUENCIES="${AUX_WRITE_FREQUENCIES_CTX}"
else
  export ACCEL_AUX_WRITE_FREQUENCIES="${ACCEL_AUX_WRITE_FREQUENCIES:-0,1,10,50}"
fi
export ACCEL_AUX_BENCH_SUITES="${ACCEL_AUX_BENCH_SUITES:-${AUX_BENCH_SUITES}}"
[[ -n "${ACCEL_AUX_BENCH_SUITES}" ]] || emit_result "${OUT}/aux_bench_sweep.json" aux_bench_sweep fail \
  "the workload names no write suite: set aux_bench_sweep.suites in its workload config" "" "query:=${QUERY}"
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

# The acceptance sweep draws its write frequencies from the suite's write_mix.yaml (0% always), anew for each of
# bench_repeats repeats; the seeds are recorded. Optimization sweeps and trees without a suite use the fixed list.
# A bench that runs several points in one process (host.sh info: bench_multi_point) gets all the points of every
# repeat in one call (ACCEL_AUX_WRITE_FREQUENCIES, no percent argument) and builds once; the rows come back in order
# and are split into the repeats. Any other bench is called once per point, the percent as its argument.
SWEEP_LOG="${OUT}/aux_bench_sweep.log"
mkdir -p "${ACCEL_AUX_BENCH_WORK_DIR}"
: > "${SWEEP_LOG}"
SWEEP_RC=0
DRAWS_JSON="${OUT}/aux_bench_draws.json"
RESULT_CSVS=()
FREQ_COUNTS=()
DRAWS=()
ALL_FREQS=()
REPEATS=1
if [[ "${CHANGE_KIND}" != "optimization" ]]; then
  REPEATS="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('test_suite', {}).get('config', {}).get('bench_repeats', 1))" "${CTX}")"
fi
IFS=',' read -r -a SUITE_LIST <<< "${ACCEL_AUX_BENCH_SUITES}"
unset "ACCEL_Q${QUERY}_WRITE_FREQUENCY_PCT"   # the point is the CLI argument (query_host_bootstrap.md)
for ((repeat = 1; repeat <= REPEATS; repeat++)); do
  DRAW=""
  if [[ "${CHANGE_KIND}" != "optimization" ]]; then
    DRAW="$(python3 "${SUITE_GATE}" "${ROOT}" mix acceptance)"
    [[ -n "${DRAW}" || ${repeat} -eq 1 ]] || break   # no suite: the fixed list, once
  fi
  if [[ -n "${DRAW}" ]]; then
    EXPECTED_FREQS="$(python3 -c "import json,sys; print(','.join(map(str, json.loads(sys.argv[1])['frequencies'])))" "${DRAW}")"
  else
    EXPECTED_FREQS="${ACCEL_AUX_WRITE_FREQUENCIES}"
    DRAW="{\"frequencies\": [${EXPECTED_FREQS}]}"
  fi
  echo "sweep ${repeat}/${REPEATS}: write frequencies ${EXPECTED_FREQS} (${DRAW})" | tee -a "${SWEEP_LOG}"
  IFS=',' read -r -a FREQ_LIST <<< "${EXPECTED_FREQS// /}"
  ALL_FREQS+=("${FREQ_LIST[@]}")
  RESULT_CSVS+=("${ACCEL_AUX_BENCH_WORK_DIR}/results_${repeat}.csv")
  FREQ_COUNTS+=("${#FREQ_LIST[@]}")
  DRAWS+=("${DRAW}")
done
POINTS_PER_CALL=1
[[ "$(engine_trait bench_multi_point)" != True ]] || POINTS_PER_CALL=${#ALL_FREQS[@]}
for csv in "${RESULT_CSVS[@]}"; do : > "${csv}"; done
for suite in "${SUITE_LIST[@]}"; do
  export "ACCEL_Q${QUERY}_BENCH_SUITE=${suite}"
  SUITE_ROWS="${ACCEL_AUX_BENCH_WORK_DIR}/rows_${suite}.csv"
  : > "${SUITE_ROWS}"
  for ((first = 0; first < ${#ALL_FREQS[@]}; first += POINTS_PER_CALL)); do
    points=("${ALL_FREQS[@]:first:POINTS_PER_CALL}")
    export ACCEL_AUX_WRITE_FREQUENCIES="$(IFS=,; echo "${points[*]}")"
    percent=()
    [[ ${POINTS_PER_CALL} -gt 1 ]] || percent=("${points[0]}")
    engine_measure "${BENCH_TARGET}" --query "${QUERY}" "${percent[@]}"
    printf '%s\n' "${LOG}" >> "${SWEEP_LOG}"
    [[ ${RC} -eq 0 ]] || SWEEP_RC=${RC}
    # keep the CSV rows (one header), drop banners
    printf '%s\n' "${LOG}" | awk -F, -v hdr="$([[ -s "${SUITE_ROWS}" ]] && echo 1 || echo 0)" '
      /^suite,/ { if (!hdr) { print; hdr = 1 }; next }
      /^speedup_gate/ || /^#/ { next }
      NF >= 10 { print }' >> "${SUITE_ROWS}"
  done
  # the suite's rows in order: one header, then each repeat's rows
  offset=1
  for i in "${!RESULT_CSVS[@]}"; do
    [[ -s "${RESULT_CSVS[i]}" ]] || head -n1 "${SUITE_ROWS}" >> "${RESULT_CSVS[i]}"
    sed -n "$((offset + 1)),$((offset + FREQ_COUNTS[i]))p" "${SUITE_ROWS}" >> "${RESULT_CSVS[i]}"
    offset=$((offset + FREQ_COUNTS[i]))
  done
done
printf '[%s]' "$(IFS=,; echo "${DRAWS[*]}")" > "${DRAWS_JSON}"

# Two extra read-only points (first repeat, first suite): at one thread, and at the thread count the pipeline
# configures for runs (ACCEL_AUX_THREADS, else the worker's CPUs) on that many CPUs of the worker's core set, not
# on the lease's one-core slot. Report-only: the speedup of N threads over 1, reads and build.
THREADS_N=1
THREADS1_CSV="${ACCEL_AUX_BENCH_WORK_DIR}/results_threads1.csv"
THREADSN_CSV="${ACCEL_AUX_BENCH_WORK_DIR}/results_threadsN.csv"
: > "${THREADS1_CSV}"
: > "${THREADSN_CSV}"
thread_point() {   # <threads> <csv>: the row of the read-only point at that thread count
  ACCEL_AUX_THREADS="$1" engine_measure "${BENCH_TARGET}" --query "${QUERY}" 0
  printf '%s\n' "${LOG}" >> "${SWEEP_LOG}"
  printf '%s\n' "${LOG}" | awk -F, '
    /^suite,/ { if (!hdr) { print; hdr = 1 }; next }
    /^speedup_gate/ || /^#/ { next }
    NF >= 10 { print }' >> "$2"
}
if [[ "${ACCEL_AUX_THREAD_POINT:-1}" != 0 && "${CHANGE_KIND}" != "optimization" ]]; then
  read -r PIN_CPUS THREADS_N <<<"$(worker_cpus "$(gate_threads)")"
  if [[ ${THREADS_N} -gt 1 ]]; then
    export "ACCEL_Q${QUERY}_BENCH_SUITE=${SUITE_LIST[0]}"
    export ACCEL_AUX_WRITE_FREQUENCIES=0
    echo "thread points: read-only at 1 thread and at ${THREADS_N} threads (cpus ${PIN_CPUS})" | tee -a "${SWEEP_LOG}"
    thread_point 1 "${THREADS1_CSV}"
    ACCEL_RUN_CPUS="${PIN_CPUS}" thread_point "${THREADS_N}" "${THREADSN_CSV}"
  fi
fi

python3 - <<PY
import json
import os
import pathlib
import sys

sys.path.insert(0, "${ROOT}/pipeline/orchestrator")
from accel_pipeline.optimization.bench import aggregate_sweep_repeats, summarize_sweep_csv, thread_scaling
from accel_pipeline.optimization.loop import record_aux_bench_sweep_outcome
from accel_pipeline.config import load_workload

results_csvs = [pathlib.Path(p) for p in "${RESULT_CSVS[*]}".split()]
results_csv = results_csvs[0]
log_path = pathlib.Path("${SWEEP_LOG}")
log_tail = log_path.read_text(encoding="utf-8")[-3000:] if log_path.is_file() else ""

summaries = [
    summarize_sweep_csv(p) if p.is_file() else {"status": "missing_csv", "row_count": 0, "composite_score": 0.0}
    for p in results_csvs
]
draws = json.loads(pathlib.Path("${DRAWS_JSON}").read_text(encoding="utf-8"))
summary = aggregate_sweep_repeats(summaries, draws) if len(summaries) > 1 else summaries[0]

status = "pass"
notes = []
expected_rows = [int(n) for n in "${FREQ_COUNTS[*]}".split()]
if int("${SWEEP_RC}") != 0:
    status = "fail"
    notes.append("bench sweep script failed")
printed = next((line for line in log_tail.splitlines() if line.startswith("suite,")), "")
for one, expected in zip(summaries, expected_rows):
    if one.get("row_count", 0) < expected:
        status = "fail"
        notes.append(
            f"expected {expected} sweep rows, got {one.get('row_count', 0)}: the bench must print one row per write "
            "frequency of ACCEL_AUX_WRITE_FREQUENCIES, at least 10 columns under the header of bench_sweep_contract.md"
            + (f" (it printed the header `{printed}`)" if printed else " (it printed no `suite,` header)"))

threads1_csv, threadsn_csv = pathlib.Path("${THREADS1_CSV}"), pathlib.Path("${THREADSN_CSV}")
scaling = {}
if threads1_csv.stat().st_size and threadsn_csv.stat().st_size:
    scaling = thread_scaling(threadsn_csv, threads1_csv, int("${THREADS_N}"), "${SUITE_LIST[0]}")
    summary["thread_scaling"] = scaling

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
    "repeats": summary.get("repeats") or draws,
    "score_spread": summary.get("score_spread", {}),
    "thread_scaling": scaling,
    "reason": "; ".join(dict.fromkeys(notes)),
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
        workload = load_workload(workload_name, pathlib.Path("${ROOT}"), engine="${ENGINE_NAME}")
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
