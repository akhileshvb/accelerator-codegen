#!/usr/bin/env bash
# Create accel_tpch_test with small TPC-H data and accelerator extension.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi
PG_HOST="${PG_HOST:-127.0.0.1}"
PG_PORT="${PG_PORT:-5434}"
PG_USER="${PG_USER:-postgres}"
DB="${TPCH_PG_DB:-accel_tpch_test}"
DUCKDB="${TPCH_TEST_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_test.duckdb}"
EXTENSION_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so}"
EXTENSION_SQL="${ROOT}/src/aux_state/engine/postgres/accelerator_codegen_postgres--0.1.sql"

export PGHOST="$PG_HOST"
export PGPORT="$PG_PORT"
export PGUSER="$PG_USER"

psql_cmd() {
  PGPASSWORD="${PGPASSWORD:-postgres}" psql -v ON_ERROR_STOP=1 "$@"
}

echo "=== building small TPC-H DuckDB test data ==="
"${PYTHON}" "${ROOT}/scripts/create_tpch_test_db.py" --dest "$DUCKDB" --force

echo "=== recreating PostgreSQL database: $DB ==="
psql_cmd -d postgres -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '${DB}' AND pid <> pg_backend_pid();" >/dev/null || true
psql_cmd -d postgres -c "DROP DATABASE IF EXISTS \"${DB}\";"
psql_cmd -d postgres -c "CREATE DATABASE \"${DB}\";"

echo "=== creating TPC-H schema ==="
psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_tpch_schema.sql"

echo "=== loading TPC-H test data from DuckDB ==="
"${PYTHON}" "${ROOT}/scripts/load_tpch_test_postgres.py" --duckdb "$DUCKDB" --db "$DB"

echo "=== installing accelerator_codegen_postgres extension SQL ==="
sed "s|MODULE_PATHNAME|${EXTENSION_SO}|g" "$EXTENSION_SQL" | psql_cmd -d "$DB" -f -

echo "=== creating mutation capture triggers ==="
psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_accel_mutation_triggers.sql"

echo "=== TPC-H PostgreSQL test database ready: $DB ==="
psql_cmd -d "$DB" -c "SELECT 'lineitem' AS table, COUNT(*) FROM lineitem UNION ALL SELECT 'orders', COUNT(*) FROM orders UNION ALL SELECT 'supplier', COUNT(*) FROM supplier UNION ALL SELECT 'partsupp', COUNT(*) FROM partsupp;"
