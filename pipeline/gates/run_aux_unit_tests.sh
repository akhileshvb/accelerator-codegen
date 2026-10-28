#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_unit_tests.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
mkdir -p "${OUT}"

# Scope by benchmark prefix (a bare "q${N}_" would match every benchmark's q<N>); keep the aux smoke test, drop the
# base kernel's.
PATTERN="${BENCHMARK_NAME}_q${QUERY}_"
EXCLUDE="^${BENCHMARK_NAME}_q${QUERY}_smoke_test$"

thread_lint
if [[ ${THREAD_LINT_RC} -ne 0 ]]; then
  emit_result "${OUT}/aux_unit.json" aux_unit fail \
    "hard-coded thread count or hardware_concurrency in the query sources: take caps from the substrate thread budget (ACCEL_AUX_THREADS ...)" \
    "${THREAD_LINT_LOG}" "query:=${QUERY}"
fi

if ! engine_ready_for_ctest "${PATTERN}" "${EXCLUDE}"; then
  engine_not_ready "${OUT}/aux_unit.json" aux_unit "query:=${QUERY}"
fi
engine_run ctest --test-dir "${ACCEL_BUILD_DIR}" --no-tests=error -j "${ACCEL_CTEST_JOBS:-$(nproc)}" --stop-on-failure -R "${PATTERN}" -E "${EXCLUDE}" --output-on-failure

# the shared result format (reason, owner) like every gate: the failing tests are the reason
[[ ${RC} -eq 0 ]] || emit_result "${OUT}/aux_unit.json" aux_unit fail \
  "ctest failed: $(sed -n '/The following tests FAILED/,$p' <<<"${LOG}" | sed -n 's/^[[:space:]]*[0-9]* - \([^ ]*\).*/\1/p' | paste -sd, -)" \
  "${ENGINE_LOG}${LOG}" "query:=${QUERY}" "exit_code:=${RC}" "pattern=${PATTERN}" "exclude=${EXCLUDE}"
emit_result "${OUT}/aux_unit.json" aux_unit pass "" "${ENGINE_LOG}${LOG}" "query:=${QUERY}" "pattern=${PATTERN}" "exclude=${EXCLUDE}"
