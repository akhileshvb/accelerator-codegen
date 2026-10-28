#!/usr/bin/env bash
# The workload harnesses honour their contract: one check on a real registered query (the declining module at the
# start, the query's own later), with every read forced to the fallback SQL so the reference answers whatever module
# is registered. Every answer is correct by construction, so a harness that fails here is wrong, and the failure goes
# to host_bootstrap, not to the query. The differential, multi-connection, cross-accelerator and bench harnesses run
# side by side on one instance as aux_exit runs them; then the bench alone with its reads hitting, and the calibrator.
# Each failure names its clause (harness_conformance.py; the clauses are stated in the contract prompts). It runs at
# the bootstrap, when the suite freezes and after every host_bootstrap revision.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
[[ -n "${QUERY}" ]] || { echo "usage: run_harness_conformance.sh <query_number>" >&2; exit 1; }
export ACCEL_GATE_TIER=quick
export ACCEL_AUX_FORCE_FALLBACK=1

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/suite_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"
CHECKS="$(dirname "${BASH_SOURCE[0]}")/harness_conformance.py"

JSON="${GATE_OUT}/harness_conformance.json"
TARGETS=("${DIFF_BINARY}" "${MULTI_CONN_TARGET}" "${CROSS_ACCEL_TARGET}" "${HARNESS_BENCH_TARGET}" "${HARNESS_CALIBRATE_TARGET}")
for target in "${TARGETS[@]}"; do
  [[ -n "${target}" ]] || emit_result "${JSON}" harness_conformance fail "a harness target is missing from the gate context. ${HOST_HINT}" "" "owner=harness"
done
engine_ready "${TARGETS[@]}" || engine_not_ready "${JSON}" harness_conformance

suite_prepare conformance "${JSON}" harness_conformance
suite_adversarial_env
[[ -z "${ACCEL_SUITE_REPORT:-}" ]] || rm -f "${ACCEL_SUITE_REPORT}"
export ACCEL_MULTI_CONN_TEST_QUERIES="${QUERY}" ACCEL_CROSS_ACCEL_TEST_QUERIES="${QUERY}" ACCEL_AUX_BENCHMARK="${BENCHMARK_NAME}"
export ACCEL_BENCH_WARMUP=2 ACCEL_BENCH_QUERIES=5 ACCEL_BENCH_REF_SAMPLES=1
export ACCEL_INTERFERENCE_CALIB_SEC=1 ACCEL_INTERFERENCE_ISOLATED_SAMPLES=5
IFS=, read -r SUITE _ <<<"${AUX_BENCH_SUITES}"
[[ -z "${SUITE}" ]] || export "ACCEL_Q${QUERY}_BENCH_SUITE=${SUITE}"

# Two read-only points: no write is needed, and a bench that ignores the list prints one row.
POINTS="0,0"
MULTI_POINT="$(engine_trait bench_multi_point)"

RUNS="${GATE_OUT}/harness_conformance"
rm -rf "${RUNS}"
mkdir -p "${RUNS}"

# The bench at every point of POINTS: one call with the list when the host says bench_multi_point, else one per point.
bench_points() {
  local out="" rc=0 reason="" point
  without_suite
  if [[ "${MULTI_POINT}" == True ]]; then
    ACCEL_AUX_WRITE_FREQUENCIES="${POINTS}" engine_run "${HARNESS_BENCH_TARGET}" --query "${QUERY}"
    return
  fi
  for point in ${POINTS//,/ }; do
    ACCEL_AUX_WRITE_FREQUENCIES="${point}" engine_run "${HARNESS_BENCH_TARGET}" --query "${QUERY}" "${point}"
    out+="${LOG}"$'\n'
    [[ ${RC} -eq 0 ]] || { rc=${RC}; reason="${RUN_REASON}"; break; }
  done
  LOG="${out}" RC=${rc} RUN_REASON="${reason}"
}

# The bench and the calibrator run as the bench sweep and the calibration gate run them: without the suite.
without_suite() { unset "${!ACCEL_SUITE_@}"; }
calibrate() { without_suite; engine_run "${HARNESS_CALIBRATE_TARGET}" --query "${QUERY}" "${RUNS}/cost_model.json"; }

# launch <name> <command...>: run it in the background, keeping its output and outcome under <RUNS>/<name>.*
PIDS=()
launch() {
  local name="$1"
  shift
  (
    ACCEL_GATE_OUT="${RUNS}/${name}.json"
    "$@"
    printf '%s' "${LOG}" >"${RUNS}/${name}.log"
    printf '%s' "${RUN_REASON}" >"${RUNS}/${name}.reason"
    echo "${RC}" >"${RUNS}/${name}.rc"
  ) &
  PIDS+=($!)
}
settle() {
  wait "${PIDS[@]}" || true
  PIDS=()
}

window_opened() {
  local line
  while read -r -t 0.2 -u 3 line; do [[ "${line}" != *enable* ]] || return 0; done
  return 1
}

LOGS=""
fail() { # clause message: the harness is wrong
  emit_result "${JSON}" harness_conformance fail "$1" "${LOGS}" "owner=harness" "${@:2}"
}
failed_run() { # name clause: fail the gate when that run exited non-zero
  local name="$1" rc reason
  rc="$(cat "${RUNS}/${name}.rc" 2>/dev/null || echo 1)"
  LOGS+="[${name}] rc=${rc}"$'\n'"$(cat "${RUNS}/${name}.log" 2>/dev/null)"$'\n'
  [[ ${rc} -ne 0 ]] || return 0
  [[ ! -f "${RUNS}/${name}.run.json" ]] || mv "${RUNS}/${name}.run.json" "${JSON%.json}.run.json"
  reason="$(cat "${RUNS}/${name}.reason" 2>/dev/null)"
  fail "clause $2: ${reason:-${name} exited ${rc}} on query ${QUERY} with every read answered by the reference SQL" \
    "target=${name}" "exit_code:=${rc}"
}

# run_profile's window (ACCEL_PROFILE_CONTROL): the gate holds the fifo open both ways, so a bench's writes never block
# and what it sent can be read back after the runs.
PROFILE_FIFO="${RUNS}/profile.ctl"
mkfifo "${PROFILE_FIFO}"
exec 3<>"${PROFILE_FIFO}"

SINGLE_RUN_MB="${ACCEL_ENGINE_MEMORY_MB}"
export ACCEL_ENGINE_MEMORY_MB="$(engine_memory_budget 4)"  # four runs side by side
for target in "${DIFF_BINARY}" "${MULTI_CONN_TARGET}" "${CROSS_ACCEL_TARGET}"; do
  launch "${target}" engine_run "${target}" "${QUERY}"
done
ACCEL_PROFILE_CONTROL="${PROFILE_FIFO}" launch bench_forced bench_points
settle
export ACCEL_ENGINE_MEMORY_MB="${SINGLE_RUN_MB}"
failed_run "${DIFF_BINARY}" differential
failed_run "${MULTI_CONN_TARGET}" concurrent
failed_run "${CROSS_ACCEL_TARGET}" concurrent
failed_run bench_forced bench-run
[[ -z "${ACCEL_SUITE_DIR:-}" || -f "${ACCEL_SUITE_REPORT}" ]] ||
  fail "clause suite-report: the differential passed with ACCEL_SUITE_DIR set but wrote no suite report at ACCEL_SUITE_REPORT (host_contract.md, Test suite)"
window_opened ||
  fail "clause bench-profile-window: the bench never opened run_profile's window: wrap its measured read loop in driver::ProfileWindow (ACCEL_PROFILE_CONTROL)"
message="$(python3 "${CHECKS}" bench-rows "${RUNS}/bench_forced.log" "${POINTS}")"
[[ -z "${message}" ]] || fail "${message}"

# The bench alone with its reads hitting, and the calibrator.
unset ACCEL_AUX_FORCE_FALLBACK
launch bench_hitting bench_points
settle
failed_run bench_hitting bench-run
message="$(python3 "${CHECKS}" bench-hits "${RUNS}/bench_hitting.log" "${RUNS}/bench_forced.log")"
[[ -z "${message}" ]] || fail "${message}"
launch calibrate calibrate
settle
failed_run calibrate calibrate-cost-model
message="$(python3 "${CHECKS}" cost-model "${RUNS}/cost_model.json")"
[[ -z "${message}" ]] || fail "${message}"

emit_result "${JSON}" harness_conformance pass "the harnesses honour their contract on query ${QUERY}" "${LOGS}"
