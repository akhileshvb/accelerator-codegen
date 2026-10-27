#!/usr/bin/env bash
# The workload harnesses on a real query before any accelerator depends on them: the differential (with the query's
# frozen suite), the multi-connection stress and the cross-accelerator test, on the subset dataset, against the module
# the test_suite stage registered, which declines every call so the reference SQL answers. Every answer is correct by
# construction, so a harness that fails here is wrong (it ignores the suite, throws on a write that leaves the answer
# unchanged, writes a read-only fixture, ...): the failure goes to host_bootstrap, not to the query.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
[[ -n "${QUERY}" ]] || { echo "usage: run_harness_acceptance.sh <query_number>" >&2; exit 1; }
export ACCEL_GATE_TIER=quick

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/suite_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/harness_acceptance.json"
for target in "${DIFF_BINARY}" "${MULTI_CONN_TARGET}" "${CROSS_ACCEL_TARGET}"; do
  [[ -n "${target}" ]] || emit_result "${JSON}" harness_acceptance fail "a harness target is missing from the gate context. ${HOST_HINT}" ""
done
engine_ready "${DIFF_BINARY}" "${MULTI_CONN_TARGET}" "${CROSS_ACCEL_TARGET}" ||
  emit_result "${JSON}" harness_acceptance fail "${ENGINE_FAIL}" "${ENGINE_LOG}"

suite_prepare quick "${JSON}" harness_acceptance
suite_adversarial_env
export ACCEL_MULTI_CONN_TEST_QUERIES="${QUERY}" ACCEL_CROSS_ACCEL_TEST_QUERIES="${QUERY}" ACCEL_AUX_BENCHMARK="${BENCHMARK_NAME}"
LOGS=""
for target in "${DIFF_BINARY}" "${MULTI_CONN_TARGET}" "${CROSS_ACCEL_TARGET}"; do
  engine_run "${target}" "${QUERY}"
  LOGS+="[${target}] rc=${RC}"$'\n'"${LOG}"$'\n'
  [[ ${RC} -eq 0 ]] || emit_result "${JSON}" harness_acceptance fail \
    "${RUN_REASON:-${target} exited ${RC}} on query ${QUERY} with the declining module, whose answers are the reference SQL's: the harness is wrong" \
    "${LOGS}" "target=${target}" "exit_code:=${RC}"
done
emit_result "${JSON}" harness_acceptance pass "the differential, multi-connection and cross-accelerator harnesses pass on query ${QUERY}" "${LOGS}"
