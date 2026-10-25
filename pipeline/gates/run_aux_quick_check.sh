#!/usr/bin/env bash
# Quick check of an aux accelerator: incremental build, the query's own tests, then the
# SQL-vs-aux differential on the subset dataset. For the agent's inner loop; the
# aux_correctness gate stays the stage-exit check.
#   ACCEL_QUICK_TARGETS   more targets to build and run (the agent's own test binaries)
#   ACCEL_QUICK_DIFFERENTIAL=0   skip the differential
#   ACCEL_QUICK_SANITIZER=thread|address   build the targets with that sanitizer (host.sh build, ACCEL_CMAKE_ARGS)
#                         in a build dir of its own, run everything with every phase at ACCEL_QUICK_THREADS threads
#                         (the substrate's ACCEL_AUX_*THREADS variables) and report sanitizer findings: only reports
#                         with a frame in the query's own directory fail the check, the rest is counted as infrastructure.
#                         A host that cannot build the variant (host.sh info embedded=false) reports "not applicable".
#                         The orchestrator points ACCEL_SUBSET_DATASET_DIR at the tiny test database for these runs.
#   ACCEL_QUICK_PREBUILD=1   with a sanitizer: only build the differential harness (substrate and harness objects) so the
#                         agent's first sanitizer check compiles just the query's files; the pipeline runs this in the
#                         background during analysis.
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
thread_lint
SANITIZER="${ACCEL_QUICK_SANITIZER:-}"
SAN_LOGDIR=""
if [[ -n "${SANITIZER}" ]]; then
  [[ "${SANITIZER}" == thread || "${SANITIZER}" == address ]] ||
    emit_result "${JSON}" quick_check fail "sanitizer must be thread or address, not ${SANITIZER}" ""
  if [[ "$(engine_trait embedded)" != "True" ]]; then
    emit_result "${JSON}" quick_check skip "sanitizer=${SANITIZER} not applicable: the host cannot build an instrumented variant (host.sh info: embedded=false)" "" "sanitizer=${SANITIZER}"
  fi
  # Its own build dir, instance and state per worker: incremental across checks, never shared with the plain build.
  export ACCEL_BUILD_DIR="${ACCEL_BUILD_DIR}-${SANITIZER}"
  export ACCEL_ENGINE_INSTANCE="${ACCEL_ENGINE_INSTANCE}-${SANITIZER}"
  export ACCEL_ENGINE_STATE_DIR="${ACCEL_ENGINE_STATE_DIR}-${SANITIZER}"
  mkdir -p "${ACCEL_ENGINE_STATE_DIR}"
  export ACCEL_CMAKE_ARGS="${ACCEL_CMAKE_ARGS} -DCMAKE_BUILD_TYPE=RelWithDebInfo -DACCEL_SANITIZE=${SANITIZER}"
  THREADS="${ACCEL_QUICK_THREADS:-4}"
  export ACCEL_AUX_THREADS="${THREADS}" ACCEL_AUX_READ_THREADS="${THREADS}" ACCEL_AUX_BUILD_THREADS="${THREADS}" \
    ACCEL_AUX_MAINT_THREADS="${THREADS}" ACCEL_AUX_TEST_GRAIN=1
  export TMPDIR="${TMPDIR:-/dev/shm}"
  SAN_LOGDIR="${GATE_OUT}/quick_check_${SANITIZER}_logs"
  rm -rf "${SAN_LOGDIR}"
  mkdir -p "${SAN_LOGDIR}"
  export TSAN_OPTIONS="suppressions=$(dirname "${BASH_SOURCE[0]}")/tsan.supp exitcode=0 second_deadlock_stack=1 log_path=${SAN_LOGDIR}/tsan"
  export ASAN_OPTIONS="detect_leaks=0 log_path=${SAN_LOGDIR}/asan"
  export UBSAN_OPTIONS="print_stacktrace=1 log_path=${SAN_LOGDIR}/ubsan"
fi
HOST_RUN="${ENGINE_HOST} run"
SAN_EXTRAS=()
sanitizer_report() { # sets SAN_EXTRAS and SAN_YOURS (reports in the query's own directory)
	SAN_YOURS=0
	SAN_EXTRAS=()
	[[ -n "${SAN_LOGDIR}" ]] || return 0
	local findings
	findings="$(python3 "$(dirname "${BASH_SOURCE[0]}")/sanitizer_findings.py" "${SAN_LOGDIR}" \
		"accelerators/${BENCHMARK_NAME}/q${QUERY}/")"
	SAN_YOURS="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['yours_count'])" "${findings}")"
	SAN_EXTRAS=("sanitizer=${SANITIZER}" "threads:=${THREADS}" "sanitizer_findings:=${findings}")
}
fail_step() { # step reproduce-command
  sanitizer_report
  emit_result "${JSON}" quick_check fail "$1 failed" "${ENGINE_LOG:-}${LOG:-}" "step=$1" \
    "reproduce=ACCEL_ENGINE_INSTANCE=${ACCEL_ENGINE_INSTANCE} ACCEL_ENGINE_STATE_DIR=${ACCEL_ENGINE_STATE_DIR} ACCEL_BUILD_DIR=${ACCEL_BUILD_DIR} ${SANITIZER:+ACCEL_QUICK_SANITIZER=${SANITIZER} ACCEL_QUICK_THREADS=${THREADS} }$2" "${SAN_EXTRAS[@]}" "${SUITE_EXTRAS[@]}"
}

if [[ "${ACCEL_QUICK_PREBUILD:-0}" == 1 ]]; then
  [[ -n "${SANITIZER}" && -n "${DIFF_BINARY}" ]] || emit_result "${JSON}" quick_check skip "nothing to prebuild" ""
  engine_ready "${DIFF_BINARY}" || fail_step build "${ENGINE_HOST} build ${DIFF_BINARY}"
  emit_result "${JSON}" quick_check pass "" "${ENGINE_LOG}"
fi

# The query's other registered ctest tests (the aux smoke test is run directly below).
PATTERN="^${BENCHMARK_NAME}_q${QUERY}_aux"
CTEST_TARGETS="$(ctest_targets "${PATTERN}" smoke | tr '\n' ' ')"

# The module's unit smoke runs on the shared test database; a sanitizer run (5-10x slower) uses the tiny one through the
# differential instead.
TARGETS=()
[[ -n "${SANITIZER}" ]] || TARGETS+=("${AUX_TEST_TARGET}")
[[ ${WITH_DIFF} -eq 1 ]] && TARGETS+=("${DIFF_BINARY}")
[[ -z "${SANITIZER}" && -n "${AUX_BENCH_TARGET}" ]] && TARGETS+=("${AUX_BENCH_TARGET}")  # read latency, counters, run_profile
read -r -a EXTRA_TARGETS <<<"${ACCEL_QUICK_TARGETS:-}"
read -r -a CTEST_TARGET_LIST <<<"${CTEST_TARGETS}"
engine_ready "${TARGETS[@]}" "${CTEST_TARGET_LIST[@]}" "${EXTRA_TARGETS[@]}" ||
  fail_step build "${ENGINE_HOST} build ${TARGETS[*]} ${CTEST_TARGETS} ${ACCEL_QUICK_TARGETS:-}"

if [[ -z "${SANITIZER}" ]]; then
  engine_run "${AUX_TEST_TARGET}"
  [[ ${RC} -eq 0 ]] || fail_step smoke "${HOST_RUN} ${AUX_TEST_TARGET}"
fi
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
  suite_prepare quick "${JSON}" quick_check
  suite_adversarial_env
  engine_run "${DIFF_BINARY}" "${QUERY}"
  suite_extras run_aux_quick_check.sh
  [[ ${RC} -eq 0 ]] || fail_step differential "${HOST_RUN} ${DIFF_BINARY} ${QUERY}"
  # As the full tier: the same differential again, same draws, capped at one thread (a sanitizer run has its own count).
  if [[ -z "${SANITIZER}" && "$(gate_threads)" -gt 1 ]]; then
    SUITE_N_EXTRAS=("${SUITE_EXTRAS[@]}")
    [[ -z "${ACCEL_SUITE_REPORT:-}" ]] || export ACCEL_SUITE_REPORT="${ACCEL_SUITE_REPORT%.json}_threads1.json"
    ACCEL_AUX_THREADS=1 engine_run "${DIFF_BINARY}" "${QUERY}"
    suite_extras run_aux_quick_check.sh
    [[ ${RC} -eq 0 ]] || fail_step differential_threads1 "ACCEL_AUX_THREADS=1 ${HOST_RUN} ${DIFF_BINARY} ${QUERY}"
    SUITE_EXTRAS=("${SUITE_N_EXTRAS[@]}")
  fi
fi
sanitizer_report
if [[ ${SAN_YOURS} -gt 0 ]]; then
  emit_result "${JSON}" quick_check fail "${SAN_YOURS} ${SANITIZER}-sanitizer report(s) with a frame in the query's own directory (see sanitizer_findings)" \
    "${ENGINE_LOG}${LOG}" "step=sanitizer" \
    "reproduce=ACCEL_QUICK_SANITIZER=${SANITIZER} ACCEL_QUICK_THREADS=${THREADS} (quick_check sanitizer=${SANITIZER} threads=${THREADS})" \
    "${SAN_EXTRAS[@]}" "${SUITE_EXTRAS[@]}"
fi
THREAD_WARNING=""
if [[ ${THREAD_LINT_RC} -ne 0 ]]; then
  THREAD_WARNING="warning: hard-coded thread count or hardware_concurrency in the query sources (aux_unit fails on it)"
  THREAD_WARNING+=$'\n'"${THREAD_LINT_LOG}"
  echo "${THREAD_WARNING}" >&2
fi
emit_result "${JSON}" quick_check pass "" "${ENGINE_LOG}${LOG}" "dataset=${ACCEL_DATASET_DIR}" "thread_lint_warning=${THREAD_WARNING}" \
  "engine_instance=${ACCEL_ENGINE_INSTANCE}" "engine_state_dir=${ACCEL_ENGINE_STATE_DIR}" \
  "${SAN_EXTRAS[@]}" "${SUITE_EXTRAS[@]}"
