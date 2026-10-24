#!/usr/bin/env bash
# Quick check of an aux accelerator: incremental build, the query's own tests, then the
# SQL-vs-aux differential on the subset dataset. For the agent's inner loop; the
# aux_correctness gate stays the stage-exit check.
#   ACCEL_QUICK_TARGETS   more targets to build and run (the agent's own test binaries)
#   ACCEL_QUICK_DIFFERENTIAL=0   skip the differential
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_quick_check.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/suite_gate_common.sh"
export ACCEL_GATE_TIER=quick
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/${ACCEL_GATE_NAME:-quick_check}.json"
WITH_DIFF=0
[[ "${BENCHMARK_NAME}" == "tpch" && "${ACCEL_QUICK_DIFFERENTIAL:-1}" != 0 ]] && WITH_DIFF=1
if [[ -z "${AUX_TEST_TARGET}" ]] || { [[ ${WITH_DIFF} -eq 1 && -z "${DIFF_BINARY}" ]]; }; then
  emit_result "${JSON}" quick_check fail "aux test / differential target missing from the gate context. ${HOST_HINT}" ""
fi
HOST_RUN="${ENGINE_HOST} run"
fail_step() { # step reproduce-command
  emit_result "${JSON}" quick_check fail "$1 failed" "${ENGINE_LOG:-}${LOG:-}" "step=$1" \
    "reproduce=ACCEL_ENGINE_INSTANCE=${ACCEL_ENGINE_INSTANCE} ACCEL_ENGINE_STATE_DIR=${ACCEL_ENGINE_STATE_DIR} ACCEL_BUILD_DIR=${ACCEL_BUILD_DIR} $2" "${SUITE_EXTRAS[@]}"
}

# The query's other registered ctest tests (the aux smoke test is run directly below).
PATTERN="^${BENCHMARK_NAME}_q${QUERY}_aux"
CTEST_TARGETS="$(ctest_targets "${PATTERN}" smoke | tr '\n' ' ')"

TARGETS=("${AUX_TEST_TARGET}")
[[ ${WITH_DIFF} -eq 1 ]] && TARGETS+=("${DIFF_BINARY}")
read -r -a EXTRA_TARGETS <<<"${ACCEL_QUICK_TARGETS:-}"
read -r -a CTEST_TARGET_LIST <<<"${CTEST_TARGETS}"
engine_ready "${TARGETS[@]}" "${CTEST_TARGET_LIST[@]}" "${EXTRA_TARGETS[@]}" ||
  fail_step build "${ENGINE_HOST} build ${TARGETS[*]} ${CTEST_TARGETS} ${ACCEL_QUICK_TARGETS:-}"

engine_run "${AUX_TEST_TARGET}"
[[ ${RC} -eq 0 ]] || fail_step smoke "${HOST_RUN} ${AUX_TEST_TARGET}"
if [[ -n "${CTEST_TARGETS// /}" ]]; then
  engine_run ctest --test-dir "${ACCEL_BUILD_DIR}" --no-tests=ignore -j "${ACCEL_CTEST_JOBS:-$(nproc)}" \
    --stop-on-failure -R "${PATTERN}" -E smoke --output-on-failure
  [[ ${RC} -eq 0 ]] || fail_step ctest "${HOST_RUN} ctest --test-dir ${ACCEL_BUILD_DIR} -R ${PATTERN} -E smoke --output-on-failure"
fi
for target in "${EXTRA_TARGETS[@]}"; do
  engine_run "${target}"
  [[ ${RC} -eq 0 ]] || fail_step "${target}" "${HOST_RUN} ${target}"
done
if [[ ${WITH_DIFF} -eq 1 ]]; then
  export ACCEL_ADV_MAX_PROBES="${ACCEL_ADV_MAX_PROBES:-${ACCEL_QUICK_PROBES:-3}}"
  suite_prepare quick "${JSON}" quick_check
  engine_run "${DIFF_BINARY}" "${QUERY}"
  suite_extras run_aux_quick_check.sh
  [[ ${RC} -eq 0 ]] || fail_step differential "${HOST_RUN} ${DIFF_BINARY} ${QUERY}"
fi
emit_result "${JSON}" quick_check pass "" "${ENGINE_LOG}${LOG}" "dataset=${ACCEL_DATASET_DIR}" "${SUITE_EXTRAS[@]}"
