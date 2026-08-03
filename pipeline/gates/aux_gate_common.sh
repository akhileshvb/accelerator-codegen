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
	export CTX ART_NS BENCHMARK_NAME AUX_TEST_TARGET AUX_BENCH_TARGET AUX_CALIBRATE_TARGET
	export AUX_CTEST_SMOKE_REGEX BENCH_TARGET DATASET_DB DB_ENV_VAR COST_MODEL_CONFIG
	export AUX_BENCH_SUITES AUX_BENCH_WORK_DIR_SUFFIX AUX_WRITE_FREQUENCIES_CTX GATE_OUT GATE_ART
}
