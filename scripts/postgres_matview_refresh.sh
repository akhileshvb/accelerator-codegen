#!/usr/bin/env bash
# REFRESH per-query matview in accel_pg_test (replaces MV contents).
# Usage: postgres_matview_refresh.sh <query_number>
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: postgres_matview_refresh.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=../pipeline/gates/mv_gate_common.sh
source "${ROOT}/pipeline/gates/mv_gate_common.sh"
load_mv_gate_context "${ROOT}" "${QUERY}"

CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
DB="${TPCH_PG_DB:-accel_tpch_test}"

ensure_mv_postgres_ready "${ROOT}" || exit 1

REFRESH_SQL="${ROOT}/${MV_REFRESH_SQL}"
if [[ -f "${REFRESH_SQL}" ]]; then
  docker exec -i \
    -e PGPASSWORD="${PGPASSWORD:-postgres}" \
    "${CONTAINER}" \
    psql -v ON_ERROR_STOP=1 -U "${PGUSER:-postgres}" -d "${DB}" \
    < "${REFRESH_SQL}"
else
  docker exec \
    -e PGPASSWORD="${PGPASSWORD:-postgres}" \
    "${CONTAINER}" \
    psql -v ON_ERROR_STOP=1 -U "${PGUSER:-postgres}" -d "${DB}" \
    -c "REFRESH MATERIALIZED VIEW ${MV_RELATION_NAME};"
fi

echo "refreshed ${MV_RELATION_NAME} for Q${QUERY}"
