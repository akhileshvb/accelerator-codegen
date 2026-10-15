#!/usr/bin/env bash
# Shared gate-context loader for materialized_view gate scripts.
# Usage: source mv_gate_common.sh; load_mv_gate_context "${ROOT}" "${QUERY}"

load_mv_gate_context() {
	local root="$1"
	local query="$2"
	local gates_dir
	gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	CTX="$(python3 "${gates_dir}/read_gate_context.py" "${root}" "${query}")"
	ART_NS="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifact_namespace'])" "${CTX}")"
	BENCHMARK_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['benchmark_name'])" "${CTX}")"
	ENGINE_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('engine_name') or 'postgres')" "${CTX}")"
	WORKLOAD_KIND="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('workload_kind') or 'materialized_view')" "${CTX}")"
	MATVIEW_SUBDIR="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('matview_subdir') or '')" "${CTX}")"
	MV_FUNCTION_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('mv_function_name') or '')" "${CTX}")"
	MV_RELATION_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('mv_relation_name') or '')" "${CTX}")"
	MV_CREATE_SQL="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('mv_create_sql') or '')" "${CTX}")"
	MV_INDEXES_SQL="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('mv_indexes_sql') or '')" "${CTX}")"
	MV_QUERY_SQL="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('mv_query_sql') or '')" "${CTX}")"
	MV_FUNCTION_SQL="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('mv_function_sql') or '')" "${CTX}")"
	MV_REFRESH_SQL="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('mv_refresh_sql') or '')" "${CTX}")"
	MV_REFERENCE_SQL="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('mv_reference_sql') or '')" "${CTX}")"
	REFERENCE_SQL_PATH="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('reference_sql_path') or '')" "${CTX}")"

	# Standalone defaults when orchestrator has not written matview gate context yet.
	if [[ -z "${MATVIEW_SUBDIR}" ]]; then
		MATVIEW_SUBDIR="eval/bench/postgres/baselines/${BENCHMARK_NAME}/q${query}/matview"
	fi
	if [[ -z "${MV_FUNCTION_NAME}" ]]; then
		MV_FUNCTION_NAME="accel_${BENCHMARK_NAME}_q${query}_mv"
	fi
	if [[ -z "${MV_RELATION_NAME}" ]]; then
		MV_RELATION_NAME="accel_${BENCHMARK_NAME}_q${query}_matview"
	fi
	if [[ -z "${MV_CREATE_SQL}" ]]; then MV_CREATE_SQL="${MATVIEW_SUBDIR}/create.sql"; fi
	if [[ -z "${MV_INDEXES_SQL}" ]]; then MV_INDEXES_SQL="${MATVIEW_SUBDIR}/indexes.sql"; fi
	if [[ -z "${MV_QUERY_SQL}" ]]; then MV_QUERY_SQL="${MATVIEW_SUBDIR}/query.sql"; fi
	if [[ -z "${MV_FUNCTION_SQL}" ]]; then MV_FUNCTION_SQL="${MATVIEW_SUBDIR}/function.sql"; fi
	if [[ -z "${MV_REFRESH_SQL}" ]]; then MV_REFRESH_SQL="${MATVIEW_SUBDIR}/refresh.sql"; fi
	if [[ -z "${MV_REFERENCE_SQL}" ]]; then MV_REFERENCE_SQL="${MATVIEW_SUBDIR}/reference.sql"; fi

	GATE_OUT="${root}/pipeline/artifacts/${ART_NS}/q${query}/gates"
	GATE_ART="${root}/pipeline/artifacts/${ART_NS}/q${query}"
	export CTX ART_NS BENCHMARK_NAME ENGINE_NAME WORKLOAD_KIND
	export MATVIEW_SUBDIR MV_FUNCTION_NAME MV_RELATION_NAME
	export MV_CREATE_SQL MV_INDEXES_SQL MV_QUERY_SQL MV_FUNCTION_SQL MV_REFRESH_SQL MV_REFERENCE_SQL
	export REFERENCE_SQL_PATH GATE_OUT GATE_ART
	export ACCEL_ENGINE_NAME="${ENGINE_NAME}"
	export ACCEL_WORKLOAD_KIND="${WORKLOAD_KIND}"
}

ensure_mv_postgres_ready() {
	local root="$1"
	local container="${ACCEL_PG_CONTAINER:-accel_pg_test}"
	local db="${TPCH_PG_DB:-accel_tpch_test}"
	# Matview gates need TPC-H tables; do not require extension rebuild.
	# Only (re)load TPCH when lineitem is missing — avoid wiping SF1 on every gate.
	local load_tpch=0
	if ! docker exec -e PGPASSWORD="${PGPASSWORD:-postgres}" "${container}" \
		psql -U "${PGUSER:-postgres}" -d "${db}" -tAc "SELECT 1 FROM lineitem LIMIT 1" \
		>/dev/null 2>&1; then
		load_tpch=1
	fi
	set +e
	ACCEL_PG_ENSURE_BUILD=0 ACCEL_PG_ENSURE_TPCH="${load_tpch}" \
		bash "${root}/scripts/ensure_accel_pg_test.sh" >/tmp/accel_mv_ensure.log 2>&1
	local rc=$?
	set -e
	if [[ ${rc} -ne 0 ]]; then
		# Container-only success is enough when tables already exist.
		if [[ "${load_tpch}" -eq 0 ]] && docker exec "${container}" true 2>/dev/null; then
			return 0
		fi
		echo "ensure_accel_pg_test failed (see /tmp/accel_mv_ensure.log)" >&2
		tail -40 /tmp/accel_mv_ensure.log >&2 || true
		return 1
	fi
	if ! docker exec "${container}" true 2>/dev/null; then
		echo "container ${container} not running" >&2
		return 1
	fi
	return 0
}

write_mv_gate_json() {
	local gate="$1"
	local status="$2"
	local reason="$3"
	local query="$4"
	mkdir -p "${GATE_OUT}"
	python3 -c "
import json, sys
path = sys.argv[1]
payload = {
  'gate': sys.argv[2],
  'query': int(sys.argv[3]),
  'status': sys.argv[4],
  'reason': sys.argv[5],
  'workload_kind': 'materialized_view',
  'mv_relation_name': sys.argv[6],
  'mv_function_name': sys.argv[7],
}
open(path, 'w', encoding='utf-8').write(json.dumps(payload, indent=2) + '\n')
" "${GATE_OUT}/${gate}.json" "${gate}" "${query}" "${status}" "${reason}" \
  "${MV_RELATION_NAME:-}" "${MV_FUNCTION_NAME:-}"
}
