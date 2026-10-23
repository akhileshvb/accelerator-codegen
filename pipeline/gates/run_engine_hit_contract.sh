#!/usr/bin/env bash
# The engine's hit contract (host_contract.md "Hit contract"): a fixture accelerator on a database
# builds on its first read and answers the second as a hit that runs no fallback SQL. The backend's
# harness `test_<engine>_hit_contract` runs through `host.sh run` with ACCEL_FALLBACK_TRAP=1 and
# exits 0 (pass), 1 (violated) or 77 (not applicable). A backend whose `info` does not say
# "hit_contract": true predates the contract: skip.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export ROOT

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_engine_gate_context "${ROOT}" 0

GATE=engine_hit_contract
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/${GATE}.json}"
TARGET="$(python3 -c "import json,sys; print((json.loads(sys.argv[1]).get('target_paths') or {}).get('hit_contract_test_target') or sys.argv[2])" \
  "${CTX}" "test_${ENGINE_NAME}_hit_contract")"

if [[ "$(engine_trait hit_contract)" != "True" ]]; then
  emit_result "${OUT_JSON}" "${GATE}" skip "backend predates the hit contract (host.sh info has no \"hit_contract\": true)" "" \
    "engine=${ENGINE_NAME}"
fi
if ! engine_steps "up" "load ${SCHEMA_SQL}" "build ${TARGET}"; then
  emit_result "${OUT_JSON}" "${GATE}" fail "${ENGINE_FAIL}" "${ENGINE_LOG}" "engine=${ENGINE_NAME}" "cmake_target=${TARGET}"
fi

ACCEL_FALLBACK_TRAP=1 engine_run "${TARGET}"
case "${RC}" in
  0)  emit_result "${OUT_JSON}" "${GATE}" pass "${TARGET}: built on the first read, hit on the second" "${ENGINE_LOG}${LOG}" \
        "engine=${ENGINE_NAME}" "returncode:=0" ;;
  77) emit_result "${OUT_JSON}" "${GATE}" skip "${TARGET}: the engine profile declares the hit contract n/a" "${ENGINE_LOG}${LOG}" \
        "engine=${ENGINE_NAME}" "returncode:=77" ;;
  *)  emit_result "${OUT_JSON}" "${GATE}" fail "${RUN_REASON:-hit contract violated: ${TARGET} exited ${RC}}" "${ENGINE_LOG}${LOG}" \
        "engine=${ENGINE_NAME}" "returncode:=${RC}" ;;
esac
