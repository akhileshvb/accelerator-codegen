#!/usr/bin/env bash
# Create accel_sec_edgar_test with small SEC EDGAR data and accelerator extension.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi
PG_HOST="${PG_HOST:-127.0.0.1}"
PG_PORT="${PG_PORT:-5434}"
PG_USER="${PG_USER:-postgres}"
DB="${SEC_PG_DB:-accel_sec_edgar_test}"
DUCKDB="${SEC_EDGAR_TEST_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_test.duckdb}"
EXTENSION_SO="${ACCEL_POSTGRES_SO:-/tmp/accelerator_codegen_postgres.so}"
EXTENSION_SQL="${ROOT}/src/runtime/postgres/accelerator_codegen_postgres--0.1.sql"

export PGHOST="$PG_HOST"
export PGPORT="$PG_PORT"
export PGUSER="$PG_USER"

psql_cmd() {
  PGPASSWORD="${PGPASSWORD:-postgres}" psql -v ON_ERROR_STOP=1 "$@"
}

echo "=== ensuring SEC EDGAR DuckDB test data ==="
if [[ ! -f "$DUCKDB" ]]; then
  "$PYTHON" "${ROOT}/scripts/create_sec_edgar_test_db.py" --dest "$DUCKDB"
fi

echo "=== recreating PostgreSQL database: $DB ==="
psql_cmd -d postgres -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '${DB}' AND pid <> pg_backend_pid();" >/dev/null || true
psql_cmd -d postgres -c "DROP DATABASE IF EXISTS \"${DB}\";"
psql_cmd -d postgres -c "CREATE DATABASE \"${DB}\";"

echo "=== creating SEC schema ==="
psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_sec_edgar_schema.sql"

echo "=== loading SEC EDGAR test data from DuckDB ==="
"$PYTHON" "${ROOT}/scripts/load_sec_edgar_test_postgres.py" --duckdb "$DUCKDB" --db "$DB"

if [[ ! -f "$EXTENSION_SO" ]]; then
  echo "missing extension shared object: $EXTENSION_SO" >&2
  exit 1
fi

echo "=== installing accelerator_codegen_postgres extension SQL ==="
sed "s|MODULE_PATHNAME|${EXTENSION_SO}|g" "$EXTENSION_SQL" | psql_cmd -d "$DB" -f -

echo "=== creating mutation capture triggers ==="
psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_accel_mutation_triggers.sql"

echo "=== SEC EDGAR PostgreSQL test database ready: $DB ==="
psql_cmd -d "$DB" -c "SELECT 'sub' AS table, COUNT(*) FROM sub UNION ALL SELECT 'num', COUNT(*) FROM num UNION ALL SELECT 'pre', COUNT(*) FROM pre UNION ALL SELECT 'tag', COUNT(*) FROM tag;"
