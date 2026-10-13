#!/usr/bin/env bash
# Apply per-query matview SQL into accel_pg_test (CREATE MV + indexes + function).
# Usage: postgres_matview_apply.sh <query_number>
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: postgres_matview_apply.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=../pipeline/gates/mv_gate_common.sh
source "${ROOT}/pipeline/gates/mv_gate_common.sh"
load_mv_gate_context "${ROOT}" "${QUERY}"

CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
DB="${TPCH_PG_DB:-accel_tpch_test}"

ensure_mv_postgres_ready "${ROOT}" || exit 1

CREATE_SQL="${ROOT}/${MV_CREATE_SQL}"
INDEXES_SQL="${ROOT}/${MV_INDEXES_SQL}"
FUNCTION_SQL="${ROOT}/${MV_FUNCTION_SQL}"
QUERY_SQL="${ROOT}/${MV_QUERY_SQL}"

missing=0
for f in "${CREATE_SQL}" "${INDEXES_SQL}"; do
  if [[ ! -f "${f}" ]]; then
    echo "missing required SQL: ${f}" >&2
    missing=1
  fi
done
if [[ ${missing} -ne 0 ]]; then
  exit 1
fi

run_psql_file() {
  local path="$1"
  docker exec -i \
    -e PGPASSWORD="${PGPASSWORD:-postgres}" \
    "${CONTAINER}" \
    psql -v ON_ERROR_STOP=1 -U "${PGUSER:-postgres}" -d "${DB}" \
    < "${path}"
}

# Drop prior matview so re-apply is idempotent (function uses CREATE OR REPLACE).
docker exec \
  -e PGPASSWORD="${PGPASSWORD:-postgres}" \
  "${CONTAINER}" \
  psql -v ON_ERROR_STOP=1 -U "${PGUSER:-postgres}" -d "${DB}" \
  -c "DROP MATERIALIZED VIEW IF EXISTS ${MV_RELATION_NAME} CASCADE;" \
  >/dev/null

run_psql_file "${CREATE_SQL}"
run_psql_file "${INDEXES_SQL}"
if [[ -f "${FUNCTION_SQL}" ]]; then
  run_psql_file "${FUNCTION_SQL}"
elif [[ -f "${QUERY_SQL}" ]]; then
  echo "note: no function.sql; query.sql present for residual SELECT" >&2
fi

docker exec \
  -e PGPASSWORD="${PGPASSWORD:-postgres}" \
  "${CONTAINER}" \
  psql -v ON_ERROR_STOP=1 -U "${PGUSER:-postgres}" -d "${DB}" \
  -c "ANALYZE ${MV_RELATION_NAME};" \
  >/dev/null

echo "applied matview SQL for Q${QUERY} (${MV_RELATION_NAME}) into ${CONTAINER}/${DB}"
