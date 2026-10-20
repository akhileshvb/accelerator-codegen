#!/usr/bin/env bash
# Shared gate-context loader for aux-state gate scripts.
# Usage: source aux_gate_common.sh; load_aux_gate_context "${ROOT}" "${QUERY}"

# Gate context only: CTX and ENGINE_NAME (gates that need no workload fields).
load_engine_gate_context() {
	local root="$1"
	local query="$2"
	local gates_dir
	gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	CTX="$(python3 "${gates_dir}/read_gate_context.py" "${root}" "${query}")"
	ENGINE_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['engine_name'])" "${CTX}")"
	SCHEMA_SQL="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('schema_sql',''))" "${CTX}")"
	DATASET_DIR="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('dataset_dir',''))" "${CTX}")"
	export CTX ENGINE_NAME SCHEMA_SQL DATASET_DIR ACCEL_ENGINE_NAME="${ENGINE_NAME}"
	setup_engine_host "${root}"
}

load_aux_gate_context() {
	local root="$1"
	local query="$2"
	load_engine_gate_context "${root}" "${query}"
	GATE_ART="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifacts_dir'])" "${CTX}")"
	BENCHMARK_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['benchmark_name'])" "${CTX}")"
	DIFF_BINARY="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('differential_binary') or '')" "${CTX}")"
	DIFF_SOURCE="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('differential_source') or '')" "${CTX}")"
	SMOKE_READY="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print('1' if h.get('smoke_ready', True) else '0')" "${CTX}")"
	HOST_HINT="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('query_host_ready_hint') or '')" "${CTX}")"
	MULTI_CONN_TARGET="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('multi_conn_target') or '')" "${CTX}")"
	CROSS_ACCEL_TARGET="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('cross_accel_target') or '')" "${CTX}")"
	AUX_TEST_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_test_target',''))" "${CTX}")"
	AUX_BENCH_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_bench_target',''))" "${CTX}")"
	AUX_CALIBRATE_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_calibrate_target',''))" "${CTX}")"
	AUX_CTEST_SMOKE_REGEX="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_ctest_smoke_regex',''))" "${CTX}")"
	BENCH_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['bench_target'])" "${CTX}")"
	COST_MODEL_CONFIG="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('cost_model_config',''))" "${CTX}")"
	AUX_BENCH_SUITES="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_bench_suites','lineitem_only'))" "${CTX}")"
	AUX_BENCH_WORK_DIR_SUFFIX="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_bench_work_dir_suffix',''))" "${CTX}")"
	AUX_WRITE_FREQUENCIES_CTX="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_write_frequencies',''))" "${CTX}")"
	GATE_OUT="${GATE_ART}/gates"
	mkdir -p "${GATE_OUT}"
	export BENCHMARK_NAME DIFF_BINARY DIFF_SOURCE
	HARNESS_BENCH_TARGET="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('bench_target') or '')" "${CTX}")"
	HARNESS_CALIBRATE_TARGET="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('calibrate_target') or '')" "${CTX}")"
	export SMOKE_READY HOST_HINT HARNESS_BENCH_TARGET HARNESS_CALIBRATE_TARGET MULTI_CONN_TARGET CROSS_ACCEL_TARGET
	export AUX_TEST_TARGET AUX_BENCH_TARGET AUX_CALIBRATE_TARGET
	export BENCH_TARGET COST_MODEL_CONFIG AUX_CTEST_SMOKE_REGEX
	export AUX_BENCH_SUITES AUX_BENCH_WORK_DIR_SUFFIX AUX_WRITE_FREQUENCIES_CTX GATE_OUT GATE_ART
}

# Point the gate at its engine's host.sh. One instance per run and engine, so
# parallel runs never collide. Sets ENGINE_HOST; exports the host contract env.
setup_engine_host() {
	local root="$1"
	ENGINE_HOST="${root}/backends/${ENGINE_NAME}/host.sh"
	export ACCEL_ENGINE_INSTANCE="${ACCEL_ENGINE_INSTANCE:-${ACCEL_PIPELINE_WORKER_ID:-w0}-${ENGINE_NAME}}"
	export ACCEL_ENGINE_STATE_DIR="${ACCEL_ENGINE_STATE_DIR:-/flash1/$(id -un)/engine_state/${ACCEL_ENGINE_INSTANCE}}"
	export ACCEL_BUILD_DIR="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${root}/build-release}}"
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
# sets ENGINE_FAIL and returns 1; ENGINE_LOG collects every step's output.
engine_steps() {
	ENGINE_LOG=""
	local step out rc
	for step in "$@"; do
		# shellcheck disable=SC2086
		out="$(engine_host ${step} 2>&1)" && rc=0 || rc=$?
		ENGINE_LOG+="[host.sh ${step}] rc=${rc}"$'\n'"${out}"$'\n'
		if [[ ${rc} -ne 0 && ${rc} -ne 77 ]]; then
			ENGINE_FAIL="host.sh ${step%% *} failed (exit ${rc})"
			return 1
		fi
	done
}

# up + load + build for the targets.
engine_ready() { engine_steps "up" "load ${SCHEMA_SQL}" "build $*"; }

# Run a built target through the host; sets RC and LOG.
# Run a built target through host.sh with a time limit (ACCEL_GATE_RUN_TIMEOUT seconds,
# default 900), so a hung harness fails the gate instead of stalling the run. Sets LOG, RC and,
# on a timeout, RUN_REASON.
engine_run() {
	local limit="${ACCEL_GATE_RUN_TIMEOUT:-900}"
	LOG="$(timeout --kill-after=15 "${limit}" "${ENGINE_HOST}" run "$@" 2>&1)" && RC=0 || RC=$?
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
