#!/usr/bin/env bash
# Shared gate-context loader for aux-state gate scripts.
# Usage: source aux_gate_common.sh; load_aux_gate_context "${ROOT}" "${QUERY}"

# Wall seconds of each phase, one "name=seconds" line each. The EXIT trap leaves them in
# <gate json>.phases, which gates.run_gate folds into the result's phase_timings.
PHASE_LOG=""
flush_phase_log() {
	[[ -z "${ACCEL_GATE_OUT:-}" ]] || printf '%s' "${PHASE_LOG}" >"${ACCEL_GATE_OUT%.json}.phases"
}
trap flush_phase_log EXIT
now_us() { echo "${EPOCHREALTIME/./}"; }

# record_phase <name> <start_us>: add the seconds since start_us to <name>.
record_phase() {
	local us=$(($(now_us) - $2))
	PHASE_LOG+="$1=$((us / 1000000)).$(printf '%03d' $((us % 1000000 / 1000)))"$'\n'
}

# time_phase <name> <command...>: run in this shell, record its seconds, keep its status.
time_phase() {
	local name="$1" start rc=0
	shift
	start="$(now_us)"
	"$@" || rc=$?
	record_phase "${name}" "${start}"
	return "${rc}"
}

# Load the gate context in one python process: CTX, ENGINE_NAME, SCHEMA_SQL, DATASET_DIR
# and (aux gates) the query's targets and bench settings, all exported.
_load_gate_context() {
	local root="$1" query="$2" exports started gates_dir
	shift 2
	started="$(now_us)"
	gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	exports="$(python3 "${gates_dir}/read_gate_context.py" --shell "$@" "${root}" "${query}")"
	eval "${exports}"
	export ACCEL_ENGINE_NAME="${ENGINE_NAME}"
	GATE_ROOT="${root}"
	GATE_QUERY="${query}"
	setup_engine_host "${root}"
	record_phase load_context "${started}"
}

# Gate context only: CTX and ENGINE_NAME (gates that need no workload fields).
load_engine_gate_context() { _load_gate_context "$1" "$2" --engine-only; }

load_aux_gate_context() {
	_load_gate_context "$1" "$2"
	GATE_OUT="${ACCEL_GATE_OUT_DIR:-${GATE_ART}/gates}"
	mkdir -p "${GATE_OUT}"
	export GATE_OUT
}

# Point the gate at its engine's host.sh. One instance per worker and engine (ACCEL_INSTANCE_TAG is
# unique on the host), its state under the worker's build dir, so parallel runs never collide.
# Sets ENGINE_HOST; exports the host contract env.
# ACCEL_GATE_TIER=quick with ACCEL_SUBSET_DATASET_DIR set loads that small dataset into
# its own instance (<instance>-quick), so the full instance stays loaded and warm.
setup_engine_host() {
	local root="$1" tier_suffix=""
	if [[ "${ACCEL_GATE_TIER:-full}" == quick && -n "${ACCEL_SUBSET_DATASET_DIR:-}" ]]; then
		tier_suffix="-quick"
		DATASET_DIR="${ACCEL_SUBSET_DATASET_DIR}"
		export ACCEL_DATASET_DIR="${DATASET_DIR}"
	fi
	ENGINE_HOST="${root}/backends/${ENGINE_NAME}/host.sh"
	[[ -z "${tier_suffix}" || -z "${ACCEL_ENGINE_STATE_DIR:-}" ]] || ACCEL_ENGINE_STATE_DIR+="${tier_suffix}"
	export ACCEL_ENGINE_INSTANCE="${ACCEL_ENGINE_INSTANCE:-${ACCEL_INSTANCE_TAG:-${ACCEL_PIPELINE_WORKER_ID:-w0}}-${ENGINE_NAME}${tier_suffix}}"
	export ACCEL_BUILD_DIR="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${root}/build-release}}"
	export ACCEL_ENGINE_STATE_DIR="${ACCEL_ENGINE_STATE_DIR:-${ACCEL_BUILD_DIR}/engine_state/${ACCEL_ENGINE_INSTANCE}}"
	export ACCEL_DATASET_DIR="${ACCEL_DATASET_DIR:-${DATASET_DIR:-}}"
	# How the build dir is configured (host_contract.md): the root CMake's backend switch
	# (cmake/Backend.cmake: ACCELERATOR_CODEGEN_BUILD_<ENGINE>) and this engine's query sources.
	# Same default as engine_host.cmake_args.
	export ACCEL_CMAKE_ARGS="${ACCEL_CMAKE_ARGS:--G Ninja -DCMAKE_BUILD_TYPE=Release -DACCELERATOR_CODEGEN_BUILD_${ENGINE_NAME^^}=ON -DACCEL_CODEGEN_ENGINE=${ENGINE_NAME}}"
	mkdir -p "${ACCEL_ENGINE_STATE_DIR}"
}

engine_host() { "${ENGINE_HOST}" "$@"; }

# Engine trait from `host.sh info` (never from the engine name), e.g. embedded.
engine_trait() {
	engine_host info 2>/dev/null | python3 -c "import json,sys; print(json.load(sys.stdin).get(sys.argv[1], ''))" "$1"
}

# Run host.sh steps in order (each argument is one "verb args" string). On failure
# sets ENGINE_FAIL and returns 1; ENGINE_LOG collects every step's output. Builds of one
# build dir run one at a time (build_stamp.build_lock takes the same lock).
engine_steps() {
	ENGINE_LOG=""
	local step out rc started runner
	for step in "$@"; do
		started="$(now_us)"
		runner=("${ENGINE_HOST}")
		[[ "${step%% *}" != build ]] || runner=(flock "${ACCEL_BUILD_DIR}.build.lock" "${ENGINE_HOST}")
		# shellcheck disable=SC2086
		out="$("${runner[@]}" ${step} 2>&1)" && rc=0 || rc=$?
		record_phase "host_${step%% *}" "${started}"
		ENGINE_LOG+="[host.sh ${step}] rc=${rc}"$'\n'"${out}"$'\n'
		if [[ ${rc} -ne 0 && ${rc} -ne 77 ]]; then
			ENGINE_FAIL="host.sh ${step%% *} failed (exit ${rc})"
			return 1
		fi
	done
}

# Stamp of the code a build of these targets depends on (build_stamp.py): the query's files and
# the shared files changed since the commit, the targets, the cmake arguments, the build dir and the instance.
build_stamp() {
	local scope=()
	[[ -z "${BENCHMARK_NAME:-}" ]] || scope=(--query-dir "accelerators/${BENCHMARK_NAME}/q${GATE_QUERY}")
	python3 "${GATE_ROOT}/pipeline/orchestrator/accel_pipeline/build_stamp.py" "${GATE_ROOT}" "${scope[@]}" \
		"$*" "${ACCEL_CMAKE_ARGS}" "${ACCEL_BUILD_DIR}" "${ACCEL_ENGINE_INSTANCE}" 2>/dev/null
}

# up + load + build for the targets; the build is skipped when the same targets already built
# from the same stamp in this instance and the build dir is configured (ACCEL_BUILD_STAMP_CACHE=0
# turns the cache off).
engine_ready() {
	local started stamp="" stamp_file
	stamp_file="${ACCEL_ENGINE_STATE_DIR}/build_stamps/$(printf '%s' "$*" | sha1sum | cut -c1-16)"
	if [[ "${ACCEL_BUILD_STAMP_CACHE:-1}" != 0 ]]; then
		started="$(now_us)"
		stamp="$(build_stamp "$@")" || stamp=""
		record_phase build_stamp "${started}"
	fi
	if [[ -n "${stamp}" && -f "${ACCEL_BUILD_DIR}/CMakeCache.txt" && "$(cat "${stamp_file}" 2>/dev/null)" == "${stamp}" ]]; then
		engine_steps "up" "load ${SCHEMA_SQL}"
		return
	fi
	engine_steps "up" "load ${SCHEMA_SQL}" "build $*" || return 1
	[[ -z "${stamp}" ]] || { mkdir -p "${stamp_file%/*}" && printf '%s' "${stamp}" >"${stamp_file}"; }
}

# The build-dir binaries run by the ctest tests matching a regex (and not an optional exclude regex), one per
# line; a test that runs a tool (cmake -P) has none. A gate that runs `ctest` builds them first. Only a configured
# build dir lists the tests, and the listing leaves out the command of a binary that is not built yet, so the
# commands come from the add_test lines of the generated CTestTestfile.cmake.
ctest_targets() {
	local pattern="$1" exclude="${2:-}" args=()
	[[ -z "${exclude}" ]] || args=(-E "${exclude}")
	ctest --test-dir "${ACCEL_BUILD_DIR}" -N -R "${pattern}" "${args[@]}" --show-only=json-v1 2>/dev/null |
		BUILD="${ACCEL_BUILD_DIR}" python3 -c "
import json, os, re, sys
from pathlib import Path
try:
    names = [t['name'] for t in json.load(sys.stdin).get('tests', [])]
except ValueError:
    names = []
build = Path(os.environ['BUILD']).resolve()
files = ''.join(p.read_text(errors='replace') for p in build.rglob('CTestTestfile.cmake') if 'CMakeFiles' not in p.parts)
found = set()
for name in names:
    m = re.search(r'add_test\\(\"' + re.escape(name) + r'\" \"([^\"]+)\"', files)
    if m and Path(m.group(1)).resolve().parent == build:
        found.add(Path(m.group(1)).name)
print('\\n'.join(sorted(found)))
" || true
}

# engine_ready for the ctest tests matching a regex: configure first (the tests are listed once configured), then
# build the binaries those tests run.
engine_ready_for_ctest() {
	local targets
	engine_ready || return 1
	mapfile -t targets < <(ctest_targets "$@")
	[[ ${#targets[@]} -eq 0 ]] || engine_ready "${targets[@]}"
}

# Run a built target through the host; sets RC and LOG.
# Run a built target through host.sh with a time limit (ACCEL_GATE_RUN_TIMEOUT seconds,
# default 900), so a hung harness fails the gate instead of stalling the run. Sets LOG, RC and,
# on a timeout, RUN_REASON.
engine_run() { _engine_run "" "$@"; }

# engine_run for a benchmark: queues for the host-wide measurement lease (outside the time limit), runs
# pinned to the measurement CPUs, and records the queueing as the phase measurement_lease_wait.
engine_measure() { _engine_run "${ACCEL_GATE_NAME:-${GATE_QUERY:-gate}}" "$@"; }

_engine_run() {
	local lease="$1" limit="${ACCEL_GATE_RUN_TIMEOUT:-900}" started waited wrap=()
	shift
	started="$(now_us)"
	[[ -z "${lease}" ]] ||
		wrap=(env "PYTHONPATH=${ACCEL_PIPELINE_PYTHONPATH:-${GATE_ROOT}/pipeline/orchestrator}:${PYTHONPATH:-}" python3 -m accel_pipeline.measurement_lease
			--holder "${ACCEL_INSTANCE_TAG:-${ACCEL_PIPELINE_WORKER_ID:-w0}}" --reason "${lease}" --)
	LOG="$("${wrap[@]}" timeout --kill-after=15 "${limit}" "${ENGINE_HOST}" run "$@" 2>&1)" && RC=0 || RC=$?
	record_phase "run_${1##*/}" "${started}"
	if [[ -n "${lease}" ]]; then
		waited="$(sed -n 's/.*waited \([0-9.]*\) s for the measurement lease.*/\1/p' <<<"${LOG}" | head -n1)"
		[[ -z "${waited}" ]] || PHASE_LOG+="measurement_lease_wait=${waited}"$'\n'
	fi
	RUN_REASON=""
	[[ ${RC} -ne 124 && ${RC} -ne 137 ]] ||
		RUN_REASON="$1 did not finish within ${limit}s: it hangs (a deadlock, or a wait that never ends)"
}

# write_result <json> <gate> <status> <reason> <log> [key=value ...]
# Writes and prints the gate result (extras: key=string, key:=json); returns 0
# only for pass or skip.
write_result() {
	python3 - "$@" <<'PY'
import json, sys, pathlib, os
out, gate, status, reason, log, *extra = sys.argv[1:]
payload = {"gate": gate, "status": status, "reason": reason, "log_tail": log[-6000:],
           "engine_name": os.environ.get("ENGINE_NAME", "")}
for kv in extra:
    k, _, v = kv.partition("=")
    if k.endswith(":"):
        payload[k[:-1]] = json.loads(v)
    else:
        payload[k] = v
p = pathlib.Path(out)
p.parent.mkdir(parents=True, exist_ok=True)
p.write_text(json.dumps(payload, indent=2))
print(p.read_text())
sys.exit(0 if status in ("pass", "skip") else 1)
PY
}

# Same, then leave the gate with that status.
emit_result() { write_result "$@"; exit $?; }

# Engine-backend gate: build one validation target through the host and run it.
# Usage: engine_target_gate <gate> <result.json> <target_paths key> <target-name key>
engine_target_gate() {
	local gate="$1" json="$2" src_key="$3" target_key="$4" src target
	src="$(python3 -c "import json,sys; print((json.loads(sys.argv[1]).get('target_paths') or {}).get(sys.argv[2], ''))" "${CTX}" "${src_key}")"
	target="$(python3 -c "import json,sys; print((json.loads(sys.argv[1]).get('target_paths') or {}).get(sys.argv[2], ''))" "${CTX}" "${target_key}")"
	if [[ -z "${target}" ]]; then
		emit_result "${json}" "${gate}" fail "gate context target_paths has no ${target_key}" "" "engine=${ENGINE_NAME}"
	fi
	if [[ -n "${src}" && ! -f "${ROOT}/${src}" ]]; then
		emit_result "${json}" "${gate}" fail "missing test source: ${src}" "" "engine=${ENGINE_NAME}"
	fi
	if ! engine_steps "up" "build ${target}"; then
		emit_result "${json}" "${gate}" fail "${ENGINE_FAIL}" "${ENGINE_LOG}" "engine=${ENGINE_NAME}" "cmake_target=${target}"
	fi
	engine_run "${target}"
	if [[ ${RC} -ne 0 ]]; then
		emit_result "${json}" "${gate}" fail "${target} failed (exit ${RC})" "${ENGINE_LOG}${LOG}" \
			"engine=${ENGINE_NAME}" "cmake_target=${target}" "returncode:=${RC}"
	fi
	emit_result "${json}" "${gate}" pass "ran ${target}; exit 0" "${ENGINE_LOG}${LOG}" \
		"engine=${ENGINE_NAME}" "cmake_target=${target}" "returncode:=0"
}

# Threads the gate's processes may use: ACCEL_AUX_THREADS, else the CPUs of this process's affinity (the
# worker's core budget). With `measure`, one lease window of the measurement set (ACCEL_MEASURE_SET split into
# ACCEL_MEASURE_SLOTS windows), the CPUs the lease pins a benchmark to.
gate_threads() {
	if [[ -n "${ACCEL_AUX_THREADS:-}" ]]; then echo "${ACCEL_AUX_THREADS}"; return; fi
	if [[ "${1:-}" == measure && -n "${ACCEL_MEASURE_SET:-}" ]]; then
		python3 -c "
import sys
n = 0
for part in sys.argv[1].split(','):
    lo, _, hi = part.partition('-')
    n += int(hi or lo) - int(lo) + 1
print(max(1, n // max(1, int(sys.argv[2]))))" "${ACCEL_MEASURE_SET}" "${ACCEL_MEASURE_SLOTS:-1}"
		return
	fi
	nproc
}

# thread_lint: the query's sources hold no hard-coded thread counts or hardware_concurrency (the thread-budget
# lint, strict, on accelerators/<bench>/q<N>). Sets THREAD_LINT_RC and THREAD_LINT_LOG.
thread_lint() {
	THREAD_LINT_RC=0
	THREAD_LINT_LOG="$(cd "${GATE_ROOT}" && bash pipeline/gates/run_thread_budget_lint.sh --strict \
		"accelerators/${BENCHMARK_NAME}/q${GATE_QUERY}" 2>&1)" || THREAD_LINT_RC=$?
}
