#!/usr/bin/env bash
# The pipeline-owned canary suite (pipeline/canary, prompts/engine_backend/canary_contract.md):
# what an accelerator needs from the backend, under realistic conditions, judged against the engine's
# own SQL. The backend only binds it (tests/test_<engine>_canary.cpp); the checks are not its to write.
# One script serves three gates by name:
#   engine_canary        every check, the plain build
#   engine_canary_asan   every check, our code under ASan + UBSan (embedded engines)
#   engine_canary_tsan   the concurrency checks, our code under TSan (embedded engines)
# ACCEL_CANARY_CHECKS ("id id ...") and ACCEL_CANARY_OWNERS ("layer ...") select a subset (quick_layer).
# A sanitizer report fails the gate with the report in the log: whether it is in the backend's code or
# only in the engine's decides who must fix it (failure.py), never whether it counts.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export ROOT

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_engine_gate_context "${ROOT}" 0

GATE="${ACCEL_GATE_NAME:-engine_canary}"
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/${GATE}.json}"
TIER="${GATE#engine_canary}"; TIER="${TIER#_}"
ctx_target() { python3 -c "import json,sys; print((json.loads(sys.argv[1]).get('target_paths') or {}).get(sys.argv[2], ''))" "${CTX}" "$1"; }
TARGET="$(ctx_target canary_test_target)"; TARGET="${TARGET:-test_${ENGINE_NAME}_canary}"
SOURCE="backends/${ENGINE_NAME}/tests/${TARGET}.cpp"
KNOWN_GAP="$(python3 -c "import json,sys; print('1' if 'engine_canary' in (json.loads(sys.argv[1]).get('known_gaps') or []) else '')" "${CTX}")"

if [[ ! -f "${ROOT}/${SOURCE}" ]]; then
  if [[ -n "${KNOWN_GAP}" ]]; then
    emit_result "${OUT_JSON}" "${GATE}" skip "known gap: ${ENGINE_NAME} predates the canary suite (no ${SOURCE})" "" "engine=${ENGINE_NAME}"
  fi
  emit_result "${OUT_JSON}" "${GATE}" fail "missing ${SOURCE}: bind the canary suite to this backend (pipeline/templates/engine_backend/canary_binding.cpp.tpl, prompts/engine_backend/canary_contract.md)" "" "engine=${ENGINE_NAME}" "owner_layer=api"
fi

checks=() owners=()
for id in ${ACCEL_CANARY_CHECKS:-}; do checks+=(--check "${id}"); done
for layer in ${ACCEL_CANARY_OWNERS:-}; do owners+=(--owner "${layer}"); done
timeout_sec="${ACCEL_CANARY_TIMEOUT_SEC:-180}"

case "${TIER}" in
  "") ;;
  asan|tsan)
    if [[ "$(engine_trait embedded)" != "True" ]]; then
      emit_result "${OUT_JSON}" "${GATE}" skip "the ${TIER} tier instruments the in-process backend (host.sh info: embedded=false)" "" "engine=${ENGINE_NAME}"
    fi
    sanitizer=address; [[ "${TIER}" == tsan ]] && sanitizer=thread
    # Own build dir, instance and state: the instrumented build never shares the release build's.
    export ACCEL_ENGINE_INSTANCE="${ACCEL_ENGINE_INSTANCE}-${TIER}"
    export ACCEL_ENGINE_STATE_DIR="${ACCEL_ENGINE_STATE_DIR}-${TIER}"
    export ACCEL_BUILD_DIR="${ACCEL_BUILD_DIR}-${TIER}"
    export ACCEL_CMAKE_ARGS="${ACCEL_CMAKE_ARGS} -DCMAKE_BUILD_TYPE=RelWithDebInfo -DACCEL_SANITIZE=${sanitizer}"
    mkdir -p "${ACCEL_ENGINE_STATE_DIR}"
    timeout_sec="${ACCEL_CANARY_TIMEOUT_SEC:-360}"
    if [[ "${TIER}" == tsan && ${#checks[@]} -eq 0 && ${#owners[@]} -eq 0 ]]; then
      checks=(--check bulk_insert_threads --check commit_build_concurrency --check dml_incremental_hits --check multi_table_by_name)
    fi
    ;;
  *) emit_result "${OUT_JSON}" "${GATE}" fail "unknown canary tier: ${TIER}" "" ;;
esac

export TMPDIR="${TMPDIR:-/dev/shm}"
if ! engine_steps "up" "build ${TARGET}"; then
  emit_result "${OUT_JSON}" "${GATE}" fail "${ENGINE_FAIL}" "${ENGINE_LOG}" "engine=${ENGINE_NAME}" "cmake_target=${TARGET}" "owner_layer=api"
fi

REPORT_DIR="$(mktemp -d "${TMPDIR}/canary_${ENGINE_NAME}_${TIER:-plain}_XXXXXX")"
export ACCEL_CANARY_TIMEOUT_SEC="${timeout_sec}"
export ASAN_OPTIONS="detect_stack_use_after_return=1:detect_leaks=0:strict_string_checks=1"
export UBSAN_OPTIONS="print_stacktrace=1"
export TSAN_OPTIONS="suppressions=$(dirname "${BASH_SOURCE[0]}")/tsan.supp exitcode=0 second_deadlock_stack=1 log_path=${REPORT_DIR}/tsan"
ACCEL_GATE_RUN_TIMEOUT="${ACCEL_GATE_RUN_TIMEOUT:-3600}" engine_run "${TARGET}" "${checks[@]}" "${owners[@]}"

printf '%s' "${LOG}" >"${REPORT_DIR}/run.log"
python3 "$(dirname "${BASH_SOURCE[0]}")/canary_result.py" "${OUT_JSON}" "${GATE}" "${RC}" "${RUN_REASON}" "${REPORT_DIR}" "${ENGINE_NAME}" "${ROOT}"
