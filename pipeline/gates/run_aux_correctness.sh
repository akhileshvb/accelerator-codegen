#!/usr/bin/env bash
# Aux-state correctness: the aux smoke test, then the SQL-vs-aux differential (tpch),
# both built and run through the engine host. ACCEL_GATE_TIER=quick runs it on the subset
# dataset (ACCEL_SUBSET_DATASET_DIR) with at most ACCEL_QUICK_PROBES write probes; the default
# full tier is the stage-exit check. ACCEL_GATE_TIER=iterate (the gate aux_correctness_iterate, an optimization
# iteration's exit) runs the full dataset with a lighter suite, the minimum write probes and the N-thread run only.
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
WITH_DIFF=0
[[ "${BENCHMARK_NAME}" == "tpch" ]] && WITH_DIFF=1
if [[ -z "${AUX_TEST_TARGET}" ]] || { [[ ${WITH_DIFF} -eq 1 && -z "${DIFF_BINARY}" ]]; }; then
  emit_result "${JSON}" aux_correctness fail "aux test / differential target missing from the gate context. ${HOST_HINT}" ""
fi

TARGETS=("${AUX_TEST_TARGET}")
[[ ${WITH_DIFF} -eq 1 ]] && TARGETS+=("${DIFF_BINARY}")
if ! engine_ready "${TARGETS[@]}"; then
  emit_result "${JSON}" aux_correctness fail "${ENGINE_FAIL}" "${ENGINE_LOG}"
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
if [[ ${WITH_DIFF} -eq 1 ]]; then
  suite_prepare "${ACCEL_GATE_TIER:-full}" "${JSON}" "${ACCEL_GATE_NAME:-aux_correctness}"
  suite_adversarial_env
  engine_run "${DIFF_BINARY}" "${QUERY}"
  DIFF_RC=${RC}
  LOG_ALL="${LOG_ALL}"$'\n'"${LOG}"
  suite_extras run_aux_correctness.sh
  THREADS_N="$(gate_threads)"
  if [[ ${DIFF_RC} -ne 0 ]]; then
    emit_result "${JSON}" aux_correctness fail "SQL-vs-aux differential failed at threads=${THREADS_N} (${DIFF_BINARY})" "${LOG_ALL}" \
      "differential_binary=${DIFF_BINARY}" "smoke_exit_code:=${SMOKE_RC}" "differential_exit_code:=${DIFF_RC}" "${SUITE_EXTRAS[@]}"
  fi
  # Full tier: the same differential again, same draws, capped at one thread (the quick and iterate tiers stay at N).
  if [[ "${ACCEL_GATE_TIER:-full}" == full && ${THREADS_N} -gt 1 ]]; then
    SUITE_N_EXTRAS=("${SUITE_EXTRAS[@]}")
    [[ -z "${ACCEL_SUITE_REPORT:-}" ]] || export ACCEL_SUITE_REPORT="${ACCEL_SUITE_REPORT%.json}_threads1.json"
    ACCEL_AUX_THREADS=1 engine_run "${DIFF_BINARY}" "${QUERY}"
    DIFF1_RC=${RC}
    LOG_ALL="${LOG_ALL}"$'\n'"[threads=1]"$'\n'"${LOG}"
    suite_extras run_aux_correctness.sh
    if [[ ${DIFF1_RC} -ne 0 ]]; then
      emit_result "${JSON}" aux_correctness fail \
        "SQL-vs-aux differential passed at threads=${THREADS_N} but failed at threads=1 (${DIFF_BINARY}); reproduce with ACCEL_AUX_THREADS=1" \
        "${LOG_ALL}" "differential_binary=${DIFF_BINARY}" "smoke_exit_code:=${SMOKE_RC}" "differential_exit_code:=${DIFF1_RC}" \
        "threads_checked:=[${THREADS_N}, 1]" "${SUITE_EXTRAS[@]}"
    fi
    SUITE_EXTRAS=("${SUITE_N_EXTRAS[@]}")
    THREADS_CHECKED="[${THREADS_N}, 1]"
  else
    THREADS_CHECKED="[${THREADS_N}]"
  fi
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
