#!/usr/bin/env bash
# ThreadSanitizer gate for parallel read/build/maintenance code.
#   run_aux_tsan.sh <query_number> [engine]   smoke test + shared differential
#   run_aux_tsan.sh slots [engine]            the slot gates (engine_backend)
# Builds a cached ACCEL_SANITIZE=thread tree (ACCEL_TSAN_BUILD_DIR, default
# <root>/build-tsan) and runs with every phase at ACCEL_TSAN_THREADS (default 8)
# threads and ACCEL_AUX_TEST_GRAIN=1, so even one-row commits split across threads.
# Fails on a differential mismatch or a TSAN report whose racing access is in
# substrate/, backends/ or accelerators/ (tsan_classify.awk); reports whose
# accesses are inside the uninstrumented engine are counted but do not fail.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TARGET="${1:-}"
ENGINE_ARG="${2:-}"
if [[ -z "${TARGET}" ]]; then
  echo "usage: run_aux_tsan.sh <query_number|slots> [engine]" >&2
  exit 1
fi
BUILD="${ACCEL_TSAN_BUILD_DIR:-${ROOT}/build-tsan}"
THREADS="${ACCEL_TSAN_THREADS:-8}"
TIMEOUT_SECONDS="${ACCEL_TSAN_TIMEOUT:-3600}"

[[ "${ACCEL_GATE_NAME:-}" == "slot_tsan" ]] && TARGET="slots"

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
if [[ "${TARGET}" == "slots" ]]; then
  ENGINE_NAME="${ENGINE_ARG:-${ACCEL_ENGINE_NAME:-}}"
  OUT="$(dirname "${ACCEL_GATE_OUT:-${ROOT}/pipeline/artifacts/tsan/slot_tsan.json}")"
  NAME="slot_tsan"
else
  load_aux_gate_context "${ROOT}" "${TARGET}"
  [[ -n "${ENGINE_ARG}" ]] && ENGINE_NAME="${ENGINE_ARG}"
  OUT="${GATE_OUT}"
  NAME="aux_tsan"
fi
RESULT_JSON="${ACCEL_GATE_OUT:-${OUT}/${NAME}.json}"
mkdir -p "${OUT}"
LOGDIR="${OUT}/${NAME}_logs"
rm -rf "${LOGDIR}"
mkdir -p "${LOGDIR}"

STATUS="pass"
REASON=""
finish() {
  local rc=0
  [[ "${STATUS}" == "pass" || "${STATUS}" == "skip" ]] || rc=1
  python3 - "${RESULT_JSON}" "${NAME}" "${TARGET}" "${ENGINE_NAME}" "${STATUS}" "${REASON}" "${LOGDIR}" <<'PY'
import json, sys
out, gate, target, engine, status, reason, logdir = sys.argv[1:8]
json.dump({"gate": gate, "target": target, "engine_name": engine, "status": status,
           "reason": reason, "logs": logdir}, open(out, "w"), indent=2)
PY
  echo "${NAME} ${TARGET} [${ENGINE_NAME}]: ${STATUS^^} ${REASON}"
  exit "${rc}"
}

# Instrumented build, own instance and state: the sanitizer never shares the release build's.
unset ACCEL_ENGINE_INSTANCE ACCEL_ENGINE_STATE_DIR
export ACCEL_ENGINE_NAME="${ENGINE_NAME}"
setup_engine_host "${ROOT}"
export ACCEL_ENGINE_INSTANCE="${ACCEL_ENGINE_INSTANCE}-tsan"
export ACCEL_ENGINE_STATE_DIR="${ACCEL_ENGINE_STATE_DIR}-tsan"
export ACCEL_BUILD_DIR="${BUILD}"
mkdir -p "${ACCEL_ENGINE_STATE_DIR}"
if [[ "$(engine_trait embedded)" != "True" ]]; then
  STATUS="skip"
  REASON="TSAN gate runs the in-process host only (host.sh info: embedded=false)"
  finish
fi

if [[ ! -f "${BUILD}/CMakeCache.txt" ]]; then
  gen=()
  command -v ninja >/dev/null && gen=(-G Ninja)
  cmake -S "${ROOT}" -B "${BUILD}" "${gen[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DACCEL_SANITIZE=thread -DACCEL_CODEGEN_ENGINE=${ENGINE_NAME} >"${LOGDIR}/configure.log" 2>&1 || { STATUS="fail"; REASON="cmake configure failed"; finish; }
fi

export ACCEL_AUX_READ_THREADS="${THREADS}" ACCEL_AUX_BUILD_THREADS="${THREADS}" ACCEL_AUX_MAINT_THREADS="${THREADS}"
export ACCEL_AUX_TEST_GRAIN=1
export TMPDIR="${TMPDIR:-/dev/shm}"
export TSAN_OPTIONS="suppressions=$(dirname "${BASH_SOURCE[0]}")/tsan.supp exitcode=0 second_deadlock_stack=1 log_path=${LOGDIR}/tsan"

RUN_FAILED=""
run() { # label cmd...
  local label="$1"; shift
  timeout -k 10 "${TIMEOUT_SECONDS}" "$@" >"${LOGDIR}/${label}.log" 2>&1 || RUN_FAILED="${RUN_FAILED} ${label}"
}

if [[ "${TARGET}" == "slots" ]]; then
  for s in epoch_stress hits_under_writes read_after_commit txn_edges; do
    run "slot_${s}" env ACCEL_PIPELINE_BUILD_DIR="${BUILD}" ACCEL_ENGINE_NAME="${ENGINE_NAME}" ACCEL_GATE_NAME="slot_${s}" \
      ACCEL_GATE_OUT="${LOGDIR}/slot_${s}.json" bash "${ROOT}/pipeline/gates/run_slot_gates.sh"
  done
else
  if [[ -z "${AUX_TEST_TARGET}" || ( "${BENCHMARK_NAME}" == "tpch" && -z "${DIFF_BINARY}" ) ]]; then
    STATUS="fail"; REASON="aux test / differential target missing from the gate context"; finish
  fi
  engine_ready "${AUX_TEST_TARGET}" ${DIFF_BINARY:+"${DIFF_BINARY}"} || {
    printf '%s' "${ENGINE_LOG}" >"${LOGDIR}/build.log"
    STATUS="fail"; REASON="TSAN build failed (${LOGDIR}/build.log)"; finish
  }
  run smoke "${ENGINE_HOST}" run "${AUX_TEST_TARGET}"
  if [[ "${BENCHMARK_NAME}" == "tpch" ]]; then
    run differential "${ENGINE_HOST}" run "${DIFF_BINARY}" "${TARGET}"
  fi
fi

OURS=0
ENGINE_ONLY=0
for f in "${LOGDIR}"/tsan.*; do
  [[ -e "$f" ]] || continue
  read -r ours engine < <(awk -v pat="${ROOT}/(substrate|backends|accelerators)/" \
    -f "$(dirname "${BASH_SOURCE[0]}")/tsan_classify.awk" "$f")
  OURS=$((OURS + ours))
  ENGINE_ONLY=$((ENGINE_ONLY + engine))
done

if (( OURS > 0 )); then
  STATUS="fail"; REASON="${OURS} TSAN report(s) in our code (${LOGDIR}/tsan.*)"
elif [[ -n "${RUN_FAILED}" ]]; then
  STATUS="fail"; REASON="failed:${RUN_FAILED} (${LOGDIR})"
else
  REASON="(${ENGINE_ONLY} engine-only report(s) ignored)"
fi
finish
