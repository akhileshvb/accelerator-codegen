#!/usr/bin/env bash
# Shared gate-context loader for aux-state gate scripts.
# Usage: source aux_gate_common.sh; load_aux_gate_context "${ROOT}" "${QUERY}"

load_aux_gate_context() {
	local root="$1"
	local query="$2"
	local gates_dir
	gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	CTX="$(python3 "${gates_dir}/read_gate_context.py" "${root}" "${query}")"
	ART_NS="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifact_namespace'])" "${CTX}")"
	BENCHMARK_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['benchmark_name'])" "${CTX}")"
	ENGINE_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('engine_name') or 'duckdb')" "${CTX}")"
	DIFF_BINARY="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('differential_binary') or 'test_tpch_shared_differential')" "${CTX}")"
	DIFF_SOURCE="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('differential_source') or '')" "${CTX}")"
	SMOKE_READY="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print('1' if h.get('smoke_ready', True) else '0')" "${CTX}")"
	AGENT_BUILDS_HOST="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print('1' if h.get('agent_builds_query_host') else '0')" "${CTX}")"
	HOST_HINT="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('query_host_ready_hint') or '')" "${CTX}")"
	MULTI_CONN_CTEST="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('multi_conn_ctest_regex') or 'test_aux_multi_connection_stress')" "${CTX}")"
	CROSS_ACCEL_CTEST="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('cross_accel_ctest_regex') or 'test_cross_accel')" "${CTX}")"
	WORK_DB_SUFFIX="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('work_db_suffix') or '.duckdb')" "${CTX}")"
	AUX_TEST_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_test_target',''))" "${CTX}")"
	AUX_BENCH_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_bench_target',''))" "${CTX}")"
	AUX_CALIBRATE_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_calibrate_target',''))" "${CTX}")"
	AUX_CTEST_SMOKE_REGEX="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_ctest_smoke_regex',''))" "${CTX}")"
	BENCH_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['bench_target'])" "${CTX}")"
	DATASET_DB="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['dataset_db'])" "${CTX}")"
	DB_ENV_VAR="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['db_env_var'])" "${CTX}")"
	COST_MODEL_CONFIG="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('cost_model_config',''))" "${CTX}")"
	AUX_BENCH_SUITES="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_bench_suites','lineitem_only'))" "${CTX}")"
	AUX_BENCH_WORK_DIR_SUFFIX="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_bench_work_dir_suffix',''))" "${CTX}")"
	AUX_WRITE_FREQUENCIES_CTX="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('aux_write_frequencies',''))" "${CTX}")"
	GATE_OUT="${root}/pipeline/artifacts/${ART_NS}/q${query}/gates"
	GATE_ART="${root}/pipeline/artifacts/${ART_NS}/q${query}"
	export CTX ART_NS BENCHMARK_NAME ENGINE_NAME DIFF_BINARY DIFF_SOURCE
	export SMOKE_READY AGENT_BUILDS_HOST HOST_HINT MULTI_CONN_CTEST CROSS_ACCEL_CTEST WORK_DB_SUFFIX
	export AUX_TEST_TARGET AUX_BENCH_TARGET AUX_CALIBRATE_TARGET
	export AUX_CTEST_SMOKE_REGEX BENCH_TARGET DATASET_DB DB_ENV_VAR COST_MODEL_CONFIG
	export AUX_BENCH_SUITES AUX_BENCH_WORK_DIR_SUFFIX AUX_WRITE_FREQUENCIES_CTX GATE_OUT GATE_ART
	export ACCEL_ENGINE_NAME="${ENGINE_NAME}"
}

# Prepare a per-gate work database/dir for the selected engine.
# DuckDB: copy dataset .duckdb. Other engines: empty work dir (harness uses host SDK).
# Sets WORK_DB and exports ACCEL_CROSS_ACCEL_WORK_DB / ACCEL_ENGINE_NAME.
prepare_engine_work_db() {
	local build="$1"
	local stem="$2" # e.g. multi_conn_q3
	local suffix="${WORK_DB_SUFFIX:-.duckdb}"
	if [[ "${ENGINE_NAME:-duckdb}" == "duckdb" ]]; then
		WORK_DB="${build}/${stem}${suffix}"
		if [[ ! -f "${DATASET_DB}" ]]; then
			echo "dataset db missing: ${DATASET_DB}" >&2
			return 1
		fi
		cp "${DATASET_DB}" "${WORK_DB}"
	else
		WORK_DB="${build}/${stem}_${ENGINE_NAME}_work"
		mkdir -p "${WORK_DB}"
		# Optional: copy dataset path marker for harnesses that expect a file path.
		if [[ -n "${DATASET_DB}" && -e "${DATASET_DB}" ]]; then
			ln -sfn "${DATASET_DB}" "${WORK_DB}/dataset_source" || true
		fi
	fi
	export WORK_DB
	export ACCEL_CROSS_ACCEL_WORK_DB="${WORK_DB}"
	export ACCEL_ENGINE_NAME="${ENGINE_NAME}"
	export ACCEL_ENGINE_WORK_DB="${WORK_DB}"
}

# Run a postgres harness binary inside accel_pg_test. Prints the log to stdout,
# returns the binary's exit code. Sets PG_DOCKER_LOG.
run_postgres_docker_harness() {
	local binary="$1"
	local query="$2"
	shift 2
	local extra_env=("$@")
	local container="${ACCEL_PG_CONTAINER:-accel_pg_test}"
	local root
	root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
	set +e
	local ensure_log
	ensure_log="$(bash "${root}/scripts/ensure_accel_pg_test.sh" 2>&1)"
	local ensure_rc=$?
	set -e
	if [[ ${ensure_rc} -ne 0 ]]; then
		PG_DOCKER_LOG="${ensure_log}"
		return 1
	fi
	set +e
	ensure_log="${ensure_log}"$'\n'"$(ACCEL_PG_ENSURE_TPCH=1 bash "${root}/scripts/ensure_accel_pg_test.sh" 2>&1 | tail -40)"
	set -e
	export ACCEL_PG_NINJA_TARGETS="${binary}"
	set +e
	local build_log
	build_log="$(bash "${root}/scripts/build_accel_postgres_extension.sh" 2>&1)"
	local build_rc=$?
	set -e
	if [[ ${build_rc} -ne 0 ]] || ! docker exec "${container}" test -x "/accel-build/accel/${binary}"; then
		PG_DOCKER_LOG="${ensure_log}"$'\n'"${build_log}"
		return 1
	fi
	set +e
	PG_DOCKER_LOG="$(docker exec \
		-e ACCEL_PG_DOCKER=1 -e PGHOST=127.0.0.1 -e PGPORT=5432 \
		-e PGUSER="${PGUSER:-postgres}" -e PGPASSWORD="${PGPASSWORD:-postgres}" \
		-e ACCEL_ENGINE_NAME=postgres \
		-e ACCEL_WORKLOAD_KIND=aux_state \
		-e ACCEL_PG_DIFF_KIND=aux \
		-e TPCH_PG_DB="${TPCH_PG_DB:-accel_tpch_test}" \
		-e ACCEL_SRC=/accel-src \
		"${extra_env[@]}" \
		"${container}" "/accel-build/accel/${binary}" "${query}" 2>&1)"
	local rc=$?
	set -e
	return ${rc}
}

# Fail with JSON when an engine-specific harness target/ctest is missing.
# Usage: fail_missing_engine_harness "<out_json>" "<gate>" "<query>" "<what>"
fail_missing_engine_harness() {
	local out_json="$1"
	local gate_name="$2"
	local query="$3"
	local what="$4"
	local reason="missing ${what} for engine=${ENGINE_NAME}: replicate the duckdb harness for this backend (see query_host_bootstrap.md). ${HOST_HINT}"
	python3 - <<PY
import json, pathlib, sys
path = pathlib.Path("""${out_json}""")
payload = {
    "gate": """${gate_name}""",
    "query": int("""${query}"""),
    "engine_name": """${ENGINE_NAME:-}""",
    "status": "fail",
    "reason": """${reason}""",
}
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text(json.dumps(payload, indent=2))
print(path.read_text())
sys.exit(1)
PY
}
