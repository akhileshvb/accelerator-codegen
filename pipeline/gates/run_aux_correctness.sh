#!/usr/bin/env bash
# Aux-state correctness: the aux smoke test, then the SQL-vs-aux differential with the query's frozen suite,
# both built and run through the engine host. ACCEL_GATE_TIER=quick runs it on the subset
# dataset (ACCEL_SUBSET_DATASET_DIR) with at most ACCEL_QUICK_PROBES write probes; the default
# full tier is the stage-exit check. ACCEL_GATE_TIER=iterate (the gate aux_correctness_iterate, an optimization
# iteration's exit) runs the full dataset with a lighter suite, the minimum write probes and the N-thread run only.
# The full tier also runs the suite at one thread, beside the N-thread run.
# Run as the gate aux_correctness_quick, its result never overwrites aux_correctness.json.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_correctness.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/suite_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/${ACCEL_GATE_NAME:-aux_correctness}.json"
if [[ -z "${AUX_TEST_TARGET}" || -z "${DIFF_BINARY}" ]]; then
  emit_result "${JSON}" aux_correctness fail "aux test / differential target missing from the gate context. ${HOST_HINT}" "" "owner=pipeline"
fi

TARGETS=("${AUX_TEST_TARGET}" "${DIFF_BINARY}")
if ! engine_ready "${TARGETS[@]}"; then
  engine_not_ready "${JSON}" aux_correctness
fi

engine_run "${AUX_TEST_TARGET}"
SMOKE_RC=${RC}
LOG_ALL="${ENGINE_LOG}${LOG}"
if [[ ${SMOKE_RC} -ne 0 ]]; then
  emit_result "${JSON}" aux_correctness fail "aux smoke unit test failed" "${LOG_ALL}" \
    "differential_binary=${DIFF_BINARY}" "smoke_exit_code:=${SMOKE_RC}"
fi

DIFF_RC=0
THREADS_N=0
THREADS_CHECKED="[]"
suite_prepare "${ACCEL_GATE_TIER:-full}" "${JSON}" "${ACCEL_GATE_NAME:-aux_correctness}"
suite_adversarial_env
THREADS_N="$(gate_threads)"
# Full tier: the suite again, same draws, capped at one thread, beside the N-thread run. Only the parallel build,
# reads and maintenance differ with the thread count, and the suite builds, writes and reads through all of them;
# the other phases would repeat the N-thread run's checks at one thread. Without a suite every phase runs.
ONE_THREAD=0
if [[ "${ACCEL_GATE_TIER:-full}" == full && ${THREADS_N} -gt 1 ]]; then
  ONE_THREAD=1
  ONE_DIR="$(mktemp -d)"
  (
    export ACCEL_AUX_THREADS=1
    [[ -z "${ACCEL_SUITE_REPORT:-}" ]] || export ACCEL_SUITE_REPORT="${ACCEL_SUITE_REPORT%.json}_threads1.json"
    [[ -z "${ACCEL_SUITE_DIR:-}" ]] || export ACCEL_DIFF_PHASES=suite,drift
    engine_run "${DIFF_BINARY}" "${QUERY}"
    printf '%s' "${RC}" >"${ONE_DIR}/rc"
    printf '%s' "${LOG}" >"${ONE_DIR}/log"
  ) &
  ONE_PID=$!
fi
engine_run "${DIFF_BINARY}" "${QUERY}"
DIFF_RC=${RC}
LOG_ALL="${LOG_ALL}"$'\n'"${LOG}"
suite_extras run_aux_correctness.sh
SUITE_N_EXTRAS=("${SUITE_EXTRAS[@]}")
if [[ ${ONE_THREAD} -eq 1 ]]; then
  time_phase wait_threads1 wait "${ONE_PID}"
  DIFF1_RC="$(cat "${ONE_DIR}/rc" 2>/dev/null || echo 1)"
  LOG_ALL="${LOG_ALL}"$'\n'"[threads=1]"$'\n'"$(cat "${ONE_DIR}/log" 2>/dev/null)"
  rm -rf "${ONE_DIR}"
  [[ -z "${ACCEL_SUITE_REPORT:-}" ]] || export ACCEL_SUITE_REPORT="${ACCEL_SUITE_REPORT%.json}_threads1.json"
  suite_extras run_aux_correctness.sh
fi
if [[ ${DIFF_RC} -ne 0 ]]; then
  emit_result "${JSON}" aux_correctness fail "SQL-vs-aux differential failed at threads=${THREADS_N} (${DIFF_BINARY})" "${LOG_ALL}" \
    "differential_binary=${DIFF_BINARY}" "smoke_exit_code:=${SMOKE_RC}" "differential_exit_code:=${DIFF_RC}" "${SUITE_N_EXTRAS[@]}"
fi
if [[ ${ONE_THREAD} -eq 1 ]]; then
  if [[ ${DIFF1_RC} -ne 0 ]]; then
    emit_result "${JSON}" aux_correctness fail \
      "SQL-vs-aux differential passed at threads=${THREADS_N} but the suite failed at threads=1 (${DIFF_BINARY}); reproduce with ACCEL_AUX_THREADS=1 ACCEL_DIFF_PHASES=suite,drift" \
      "${LOG_ALL}" "differential_binary=${DIFF_BINARY}" "smoke_exit_code:=${SMOKE_RC}" "differential_exit_code:=${DIFF1_RC}" \
      "threads_checked:=[${THREADS_N}, 1]" "${SUITE_EXTRAS[@]}"
  fi
  SUITE_EXTRAS=("${SUITE_N_EXTRAS[@]}")
  THREADS_CHECKED="[${THREADS_N}, 1]"
else
  THREADS_CHECKED="[${THREADS_N}]"
fi

# The query must register from its own directory: the engine backend names no query.
CHECK_STARTED="$(now_us)"
BACKEND_RC=0
BACKEND_LOG="$(python3 "${ROOT}/scripts/check_backend_query_free.py" --engine "${ENGINE_NAME}" 2>&1)" || BACKEND_RC=$?
record_phase checker "${CHECK_STARTED}"
if [[ ${BACKEND_RC} -ne 0 ]]; then
  emit_result "${JSON}" aux_correctness fail \
    "per-query code in the ${ENGINE_NAME} backend: register from accelerators/${BENCHMARK_NAME}/q${QUERY}/ instead" \
    "${BACKEND_LOG}" "differential_binary=${DIFF_BINARY}"
fi
emit_result "${JSON}" aux_correctness pass "" "${LOG_ALL}" "differential_binary=${DIFF_BINARY}" \
  "smoke_exit_code:=${SMOKE_RC}" "differential_exit_code:=${DIFF_RC}" "threads_checked:=${THREADS_CHECKED}" "${SUITE_EXTRAS[@]}"
