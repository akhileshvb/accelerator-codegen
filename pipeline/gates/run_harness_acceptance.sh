#!/usr/bin/env bash
# The workload harnesses on a real query: the differential (with the query's frozen suite), the multi-connection
# stress and the cross-accelerator test, at once on the subset dataset, with every read forced to the fallback so the
# reference SQL answers. Every answer is correct by construction, so a harness that fails here is wrong (it ignores
# the suite, throws on a write that leaves the answer unchanged, holds the shared database, ...): the failure goes to
# host_bootstrap, not to the query. It runs when the suite freezes and after host_bootstrap revises a harness.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
[[ -n "${QUERY}" ]] || { echo "usage: run_harness_acceptance.sh <query_number>" >&2; exit 1; }
export ACCEL_GATE_TIER=quick
# Every read to the fallback SQL: the answers are the reference's whatever module is registered, so the check holds
# after the implementation too (host_bootstrap revising a harness mid-run).
export ACCEL_AUX_FORCE_FALLBACK=1

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
# All three at once on the one instance, as aux_exit runs them: a harness that writes the shared database instead of
# its own copy fails here. Each run keeps its output and outcome under harness_acceptance/<target>.*.
RUNS="${GATE_OUT}/harness_acceptance"
rm -rf "${RUNS}"
mkdir -p "${RUNS}"
HARNESSES=("${DIFF_BINARY}" "${MULTI_CONN_TARGET}" "${CROSS_ACCEL_TARGET}")
PIDS=()
for target in "${HARNESSES[@]}"; do
  (
    ACCEL_GATE_OUT="${RUNS}/${target}.json"
    engine_run "${target}" "${QUERY}"
    printf '%s' "${LOG}" >"${RUNS}/${target}.log"
    printf '%s' "${RUN_REASON}" >"${RUNS}/${target}.reason"
    echo "${RC}" >"${RUNS}/${target}.rc"
  ) &
  PIDS+=($!)
done
wait "${PIDS[@]}" || true
LOGS=""
for target in "${HARNESSES[@]}"; do
  RC="$(cat "${RUNS}/${target}.rc" 2>/dev/null || echo 1)"
  LOGS+="[${target}] rc=${RC}"$'\n'"$(cat "${RUNS}/${target}.log" 2>/dev/null)"$'\n'
done
for target in "${HARNESSES[@]}"; do
  RC="$(cat "${RUNS}/${target}.rc" 2>/dev/null || echo 1)"
  [[ ${RC} -ne 0 ]] || continue
  [[ ! -f "${RUNS}/${target}.run.json" ]] || mv "${RUNS}/${target}.run.json" "${JSON%.json}.run.json"
  reason="$(cat "${RUNS}/${target}.reason" 2>/dev/null)"
  emit_result "${JSON}" harness_acceptance fail \
    "${reason:-${target} exited ${RC}} on query ${QUERY} with every read answered by the reference SQL, run beside the other harnesses as aux_exit runs them: the harness is wrong" \
    "${LOGS}" "target=${target}" "exit_code:=${RC}" "owner=harness"
done
emit_result "${JSON}" harness_acceptance pass "the differential, multi-connection and cross-accelerator harnesses pass on query ${QUERY}" "${LOGS}"
