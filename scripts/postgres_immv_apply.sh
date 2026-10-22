#!/usr/bin/env bash
# Apply per-query pg_ivm IMMV into accel_pg_test.
# Usage: postgres_immv_apply.sh <query_number>
# Exit 0 with "skipped" if query has no pg_ivm-compatible definition.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: postgres_immv_apply.sh <query_number>" >&2
  exit 1
fi

CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
DB="${TPCH_PG_DB:-accel_tpch_test}"
IMMV_DIR="${ROOT}/backends/postgres/bench/baselines/tpch/q${QUERY}/immv"
REL="accel_tpch_q${QUERY}_immv"

if [[ -f "${IMMV_DIR}/unsupported.txt" ]]; then
  echo "skipped IMMV for Q${QUERY}: $(tr -d '\n' <"${IMMV_DIR}/unsupported.txt")"
  exit 0
fi
if [[ ! -f "${IMMV_DIR}/create.sql" ]]; then
  echo "missing ${IMMV_DIR}/create.sql — run scripts/generate_tpch_immvs.py" >&2
  exit 1
fi

bash "${ROOT}/scripts/ensure_pg_ivm.sh" "${DB}"

run_psql_file() {
  local path="$1"
  docker exec -i \
    -e PGPASSWORD="${PGPASSWORD:-postgres}" \
    "${CONTAINER}" \
    psql -v ON_ERROR_STOP=1 -U "${PGUSER:-postgres}" -d "${DB}" \
    < "${path}"
}

# IMMVs are tables; drop prior so re-apply is idempotent.
docker exec \
  -e PGPASSWORD="${PGPASSWORD:-postgres}" \
  "${CONTAINER}" \
  psql -v ON_ERROR_STOP=1 -U "${PGUSER:-postgres}" -d "${DB}" \
  -c "DROP TABLE IF EXISTS ${REL} CASCADE;" \
  >/dev/null

run_psql_file "${IMMV_DIR}/create.sql"

docker exec \
  -e PGPASSWORD="${PGPASSWORD:-postgres}" \
  "${CONTAINER}" \
  psql -v ON_ERROR_STOP=1 -U "${PGUSER:-postgres}" -d "${DB}" \
  -c "ANALYZE ${REL};" \
  >/dev/null

echo "applied IMMV SQL for Q${QUERY} (${REL}) into ${CONTAINER}/${DB}"
