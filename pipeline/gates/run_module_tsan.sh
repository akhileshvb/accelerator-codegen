#!/usr/bin/env bash
# Module-level ThreadSanitizer gate, the same for every engine.
#   run_module_tsan.sh <query_number>
# Runs the query's aux module under the substrate's engine-free host
# (eval/tests/tsan_module_driver.cpp: concurrent writers, readers and
# maintenance/build threads against a fake engine) in the cached
# ACCEL_SANITIZE=thread tree (ACCEL_TSAN_BUILD_DIR, default <root>/build-tsan).
# The driver always runs its toy module first (the host itself must be clean).
# Fails on a wrong read or a TSAN report whose racing access is in substrate/,
# backends/ or accelerators/. Status skip: the toy is clean and the driver has
# no case for the module (exit 77: a module needs its engine's scan to be built).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:?usage: run_module_tsan.sh <query_number>}"
BUILD="${ACCEL_TSAN_BUILD_DIR:-${ROOT}/build-tsan}"
THREADS="${ACCEL_TSAN_THREADS:-8}"
SECONDS_PER_RUN="${ACCEL_TSAN_MODULE_SECONDS:-20}"
DRIVER=tsan_module_driver

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"
MODULE="${BENCHMARK_NAME}_q${QUERY}"
RESULT_JSON="${ACCEL_GATE_OUT:-${GATE_OUT}/module_tsan.json}"
LOGDIR="$(dirname "${RESULT_JSON}")/module_tsan_logs"
rm -rf "${LOGDIR}"
mkdir -p "${LOGDIR}"

STATUS="pass"
REASON=""
finish() {
  python3 - "${RESULT_JSON}" "${MODULE}" "${ENGINE_NAME}" "${STATUS}" "${REASON}" "${LOGDIR}" <<'PY'
import json, sys
out, module, engine, status, reason, logdir = sys.argv[1:7]
json.dump({"gate": "module_tsan", "module": module, "engine_name": engine, "status": status,
           "reason": reason, "logs": logdir}, open(out, "w"), indent=2)
PY
  echo "module_tsan ${MODULE} [${ENGINE_NAME}]: ${STATUS^^} ${REASON}"
  [[ "${STATUS}" == "pass" || "${STATUS}" == "skip" ]]
  exit $?
}

if [[ ! -f "${BUILD}/CMakeCache.txt" ]]; then
  gen=()
  command -v ninja >/dev/null && gen=(-G Ninja)
  cmake -S "${ROOT}" -B "${BUILD}" "${gen[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DACCEL_SANITIZE=thread -DACCEL_CODEGEN_ENGINE="${ENGINE_NAME}" >"${LOGDIR}/configure.log" 2>&1 \
    || { STATUS="fail"; REASON="cmake configure failed (${LOGDIR}/configure.log)"; finish; }
fi
cmake --build "${BUILD}" --target "${DRIVER}" -j"${ACCEL_TSAN_JOBS:-6}" >"${LOGDIR}/build.log" 2>&1 \
  || { STATUS="fail"; REASON="TSAN build failed (${LOGDIR}/build.log)"; finish; }

export ACCEL_AUX_READ_THREADS="${THREADS}" ACCEL_AUX_BUILD_THREADS="${THREADS}" ACCEL_AUX_MAINT_THREADS="${THREADS}"
export TMPDIR="${TMPDIR:-/dev/shm}"
export TSAN_OPTIONS="suppressions=$(dirname "${BASH_SOURCE[0]}")/tsan.supp exitcode=0 second_deadlock_stack=1 log_path=${LOGDIR}/tsan"

run() { # module -> exit status of the driver
  timeout -k 10 $((SECONDS_PER_RUN + 120)) "${BUILD}/${DRIVER}" "$1" "${SECONDS_PER_RUN}" >"${LOGDIR}/$1.log" 2>&1
}

FAILED=""
run sum || FAILED="sum"
run "${MODULE}"
case $? in
  0) ;;
  77) NOT_COVERED=1 ;;
  *) FAILED="${FAILED} ${MODULE}" ;;
esac

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
elif [[ -n "${FAILED}" ]]; then
  STATUS="fail"; REASON="driver failed:${FAILED} (${LOGDIR})"
elif [[ -n "${NOT_COVERED:-}" ]]; then
  STATUS="skip"; REASON="host clean; no engine-free driver case for ${MODULE}"
else
  REASON="(${ENGINE_ONLY} engine-only report(s) ignored)"
fi
finish
