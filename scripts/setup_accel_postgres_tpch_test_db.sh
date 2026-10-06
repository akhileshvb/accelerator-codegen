#!/usr/bin/env bash
# Create accel_tpch_test with small TPC-H data in Docker Postgres (native tables).
# Extension is LOADed via absolute .so path (no CREATE EXTENSION control file required).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi
PG_HOST="${PG_HOST:-127.0.0.1}"
PG_PORT="${PG_PORT:-${PGPORT:-5434}}"
PG_USER="${PG_USER:-postgres}"
DB="${TPCH_PG_DB:-accel_tpch_test}"
DUCKDB="${TPCH_TEST_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_test.duckdb}"
EXTENSION_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so}"

export PGHOST="$PG_HOST"
export PGPORT="$PG_PORT"
export PGUSER="$PG_USER"

psql_cmd() {
  PGPASSWORD="${PGPASSWORD:-postgres}" psql -v ON_ERROR_STOP=1 "$@"
}

if ! psql_cmd -d postgres -tAc 'select 1' >/dev/null 2>&1; then
  echo "Postgres not reachable at ${PG_HOST}:${PG_PORT}; run scripts/ensure_accel_pg_test.sh" >&2
  exit 1
fi

echo "=== building small TPC-H DuckDB test data ==="
"${PYTHON}" "${ROOT}/scripts/create_tpch_test_db.py" --dest "$DUCKDB" --force

echo "=== recreating PostgreSQL database: $DB ==="
psql_cmd -d postgres -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '${DB}' AND pid <> pg_backend_pid();" >/dev/null || true
psql_cmd -d postgres -c "DROP DATABASE IF EXISTS \"${DB}\";"
psql_cmd -d postgres -c "CREATE DATABASE \"${DB}\";"

echo "=== creating TPC-H schema ==="
psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_tpch_schema.sql"

echo "=== loading TPC-H test data from DuckDB into Postgres ==="
"${PYTHON}" "${ROOT}/scripts/load_tpch_test_postgres.py" --duckdb "$DUCKDB" --db "$DB"

echo "=== registering accelerator LOAD helpers (so=${EXTENSION_SO}) ==="
psql_cmd -d "$DB" -c "LOAD '${EXTENSION_SO}';" || {
  echo "LOAD failed — rebuild with scripts/build_accel_postgres_extension.sh" >&2
  exit 1
}

echo "=== registering TPC-H accelerator SQL functions ==="
psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_register_tpch_all.sql"

if [[ -f "${ROOT}/scripts/postgres_tpch_mutation_triggers.sql" ]]; then
  echo "=== creating mutation capture triggers ==="
  # Trigger SQL may reference functions; ignore if not yet registered for this host.
  psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_tpch_mutation_triggers.sql" || true
fi

echo "=== TPC-H PostgreSQL test database ready: $DB (native storage) ==="
psql_cmd -d "$DB" -c "SELECT 'lineitem' AS table, COUNT(*) FROM lineitem UNION ALL SELECT 'orders', COUNT(*) FROM orders;"
