#!/usr/bin/env bash
# Create accel_sec_edgar_sf3 with full SEC EDGAR SF3 data and accelerator extension.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi

PG_HOST="${PG_HOST:-127.0.0.1}"
PG_PORT="${PG_PORT:-5434}"
PG_USER="${PG_USER:-postgres}"
DB="${SEC_PG_DB:-accel_sec_edgar_sf3}"
DUCKDB="${SEC_EDGAR_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
EXTENSION_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so}"
EXTENSION_SQL="${ROOT}/src/aux_state/engine/postgres/accelerator_codegen_postgres--0.1.sql"

export PGHOST="$PG_HOST"
export PGPORT="$PG_PORT"
export PGUSER="$PG_USER"

psql_cmd() {
  PGPASSWORD="${PGPASSWORD:-postgres}" psql -v ON_ERROR_STOP=1 "$@"
}

if [[ ! -f "$DUCKDB" ]]; then
  echo "missing SEC EDGAR SF3 DuckDB: $DUCKDB" >&2
  exit 1
fi

echo "=== recreating PostgreSQL database: $DB ==="
psql_cmd -d postgres -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '${DB}' AND pid <> pg_backend_pid();" >/dev/null || true
psql_cmd -d postgres -c "DROP DATABASE IF EXISTS \"${DB}\";"
psql_cmd -d postgres -c "CREATE DATABASE \"${DB}\";"

echo "=== creating SEC schema ==="
psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_sec_edgar_schema.sql"

echo "=== loading SEC EDGAR SF3 data from DuckDB (COPY for large tables) ==="
"$PYTHON" "${ROOT}/scripts/load_sec_edgar_test_postgres.py" \
  --duckdb "$DUCKDB" --db "$DB" --method auto

if [[ ! -f "$EXTENSION_SO" ]]; then
  echo "missing extension shared object: $EXTENSION_SO" >&2
  exit 1
fi

echo "=== installing accelerator_codegen_postgres extension SQL ==="
sed "s|MODULE_PATHNAME|${EXTENSION_SO}|g" "$EXTENSION_SQL" | psql_cmd -d "$DB" -f -

echo "=== creating mutation capture triggers ==="
psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_accel_mutation_triggers.sql"

echo "=== SEC EDGAR SF3 PostgreSQL database ready: $DB ==="
psql_cmd -d "$DB" -c "SELECT 'sub' AS table, COUNT(*) FROM sub UNION ALL SELECT 'num', COUNT(*) FROM num UNION ALL SELECT 'pre', COUNT(*) FROM pre UNION ALL SELECT 'tag', COUNT(*) FROM tag;"
