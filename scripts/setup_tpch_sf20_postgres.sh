#!/usr/bin/env bash
# Load TPC-H SF20 into Docker Postgres on /disk3 and register the accel extension.
#
# Storage model (important):
#   - TPC-H base tables are ordinary Postgres heap tables (native storage) via
#     scripts/postgres_tpch_schema.sql + INSERT load.
#   - tpch_sf20.duckdb is ONLY an offline data source for the copy; nothing is
#     queried from DuckDB at bench time (no FDW / foreign tables).
#   - Matviews are Postgres MATERIALIZED VIEW (native). Aux state is separate
#     accelerator storage used only by accel_tpch_q*_aux() — not the base TPCH.
#
# One-time (or when refreshing SF20):
#   bash scripts/setup_tpch_sf20_postgres.sh
#
# Options:
#   SKIP_CONTAINER=1   — do not recreate accel_pg_test
#   SKIP_LOAD=1        — skip DuckDB→Postgres copy (DB already loaded)
#   SKIP_EXTENSION=1   — skip extension rebuild
#   SKIP_INDEXES=1     — skip index creation
#   DB=accel_tpch_sf20  — target database name (default)
#
# Requires: ~100GB+ free on ACCEL_PG_DATA disk, tpch_sf20.duckdb present.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_ROOT="${DUCKDB_ROOT:-${ROOT}/../accelerator-codegen-duckdb}"
SF20_DUCKDB="${ACCEL_TPCH_SF20_DB:-${DUCKDB_ROOT}/tpch_sf20.duckdb}"
NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
PORT="${PG_PORT:-${PGPORT:-5434}}"
DB="${TPCH_PG_DB:-accel_tpch_sf20}"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
[[ -x "${PYTHON}" ]] || PYTHON=python3

export ACCEL_PG_DATA="${ACCEL_PG_DATA:-/disk3/akhilvb/accel_pg_test_data}"
export ACCEL_PG_BUILD="${ACCEL_PG_BUILD:-/disk3/akhilvb/accel_pg_build}"
export ACCEL_PG_SHM_SIZE="${ACCEL_PG_SHM_SIZE:-16g}"
export ACCEL_PG_SHARED_BUFFERS="${ACCEL_PG_SHARED_BUFFERS:-4GB}"
export PGPASSWORD="${PGPASSWORD:-postgres}"
export PGHOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PORT}"
export PGUSER="${PGUSER:-postgres}"

MIN_DISK_GB="${MIN_DISK_GB:-80}"
free_gb="$(df -BG "${ACCEL_PG_DATA}" | awk 'NR==2 {gsub(/G/,"",$4); print $4}')"
echo "setup_tpch_sf20_postgres: data=${ACCEL_PG_DATA} free_gb=${free_gb} db=${DB}"
if [[ "${free_gb}" -lt "${MIN_DISK_GB}" ]]; then
  echo "ERROR: need >= ${MIN_DISK_GB} GB free on ${ACCEL_PG_DATA}, have ${free_gb} GB" >&2
  exit 1
fi
if [[ ! -f "${SF20_DUCKDB}" ]]; then
  echo "ERROR: missing ${SF20_DUCKDB}" >&2
  echo "run: bash scripts/setup_tpch_sf20_db.sh" >&2
  exit 1
fi

psql_cmd() {
  psql -v ON_ERROR_STOP=1 "$@"
}

if [[ "${SKIP_CONTAINER:-0}" != "1" ]]; then
  echo "=== restarting ${NAME} with SF20-sized shm/buffers on ${ACCEL_PG_DATA} ==="
  bash "${ROOT}/scripts/start_accel_pg_test.sh"
fi

if ! psql_cmd -d postgres -tAc 'select 1' >/dev/null 2>&1; then
  echo "Postgres not reachable at ${PGHOST}:${PGPORT}" >&2
  exit 1
fi

if [[ "${SKIP_EXTENSION:-0}" != "1" ]]; then
  echo "=== building/installing accelerator_codegen_pg extension ==="
  bash "${ROOT}/scripts/build_accel_postgres_extension.sh"
fi

if [[ "${SKIP_LOAD:-0}" != "1" ]]; then
  echo "=== recreating database ${DB} ==="
  psql_cmd -d postgres -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '${DB}' AND pid <> pg_backend_pid();" >/dev/null || true
  psql_cmd -d postgres -c "DROP DATABASE IF EXISTS \"${DB}\";"
  psql_cmd -d postgres -c "CREATE DATABASE \"${DB}\";"

  echo "=== creating TPC-H schema (native Postgres heap tables) ==="
  psql_cmd -d "${DB}" -f "${ROOT}/scripts/postgres_tpch_schema.sql"

  echo "=== copying SF20 rows from DuckDB source into Postgres native tables (slow) ==="
  echo "    source=${SF20_DUCKDB}  (import only; not used at query time)"
  "${PYTHON}" "${ROOT}/scripts/load_tpch_sf1_postgres.py" \
    --duckdb "${SF20_DUCKDB}" \
    --db "${DB}"

  if [[ "${SKIP_INDEXES:-0}" != "1" ]]; then
    echo "=== creating TPC-H indexes (native Postgres indexes) ==="
    psql_cmd -d "${DB}" -f "${ROOT}/scripts/postgres_tpch_indexes.sql"
  fi
  echo "=== verifying native heap storage ==="
  psql_cmd -d "${DB}" -c "
    SELECT c.relname, a.amname AS access_method, c.relkind
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    JOIN pg_am a ON a.oid = c.relam
    WHERE n.nspname = 'public'
      AND c.relname IN ('lineitem','orders','customer','supplier','part','partsupp','nation','region')
    ORDER BY c.relname;
  "
else
  echo "=== SKIP_LOAD=1; expecting existing ${DB} ==="
  if ! psql_cmd -d "${DB}" -tAc "SELECT 1 FROM lineitem LIMIT 1" >/dev/null 2>&1; then
    echo "ERROR: ${DB} missing or empty lineitem" >&2
    exit 1
  fi
fi

EXTENSION_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so}"
echo "=== registering accelerator functions (LOAD ${EXTENSION_SO}) ==="
docker exec -i -e PGPASSWORD="${PGPASSWORD}" "${NAME}" \
  psql -v ON_ERROR_STOP=1 -U postgres -d "${DB}" <<SQL
LOAD '${EXTENSION_SO}';
\\i /accel-src/scripts/postgres_register_tpch_all.sql
SQL
if [[ -f "${ROOT}/scripts/postgres_accel_mutation_triggers.sql" ]]; then
  docker exec -i -e PGPASSWORD="${PGPASSWORD}" "${NAME}" \
    psql -v ON_ERROR_STOP=1 -U postgres -d "${DB}" \
    -f /accel-src/scripts/postgres_accel_mutation_triggers.sql || true
fi

echo "=== SF20 Postgres ready ==="
psql_cmd -d "${DB}" -c "
SELECT 'lineitem' AS t, COUNT(*)::bigint AS n FROM lineitem
UNION ALL SELECT 'orders', COUNT(*)::bigint FROM orders
UNION ALL SELECT 'customer', COUNT(*)::bigint FROM customer;
"
echo "Next: TPCH_PG_DB=${DB} bash scripts/run_tpch_pg_matview_vs_aux_sf20_sweep.sh"
