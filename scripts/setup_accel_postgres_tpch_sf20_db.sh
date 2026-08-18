#!/usr/bin/env bash
# Create accel_tpch_sf20 with TPC-H SF20 data + accelerator aux extension.
#
# Prerequisites:
#   - accel_pg_test listening on PG_PORT (default 5434)
#   - SF20 DuckDB at ../accelerator-codegen-duckdb/tpch_sf20.duckdb
#       (bash scripts/setup_tpch_sf20_db.sh)
#   - ~40+ GB free disk for the Postgres copy (+ temp CSV for largest table)
#
# Usage:
#   bash scripts/setup_accel_postgres_tpch_sf20_db.sh
#   FORCE=1 bash scripts/setup_accel_postgres_tpch_sf20_db.sh
#   SKIP_LOAD=1 bash scripts/setup_accel_postgres_tpch_sf20_db.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi

PG_HOST="${PG_HOST:-127.0.0.1}"
PG_PORT="${PG_PORT:-5434}"
PG_USER="${PG_USER:-postgres}"
DB="${TPCH_PG_DB:-accel_tpch_sf20}"
DUCKDB="${ACCEL_TPCH_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf20.duckdb}"
EXTENSION_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so}"
EXTENSION_SQL="${ROOT}/src/runtime/postgres/accelerator_codegen_postgres--0.1.sql"
FORCE="${FORCE:-0}"
SKIP_LOAD="${SKIP_LOAD:-0}"
MIN_DISK_GB="${MIN_DISK_GB:-40}"
STAGING_DIR="${STAGING_DIR:-}"

export PGHOST="$PG_HOST"
export PGPORT="$PG_PORT"
export PGUSER="$PG_USER"
export PGPASSWORD="${PGPASSWORD:-postgres}"

psql_cmd() {
  psql -v ON_ERROR_STOP=1 "$@"
}

pg_ready() {
  psql_cmd -d postgres -tAc 'select 1' >/dev/null 2>&1
}

reinstall_aux_functions() {
  local db="$1"
  echo "=== reinstalling aux functions on ${db} ==="
  psql_cmd -d "$db" <<'SQL'
DO $drop$
DECLARE
  fn record;
BEGIN
  FOR fn IN
    SELECT p.oid::regprocedure AS signature
    FROM pg_proc p
    WHERE p.proname LIKE 'accel\_%\_aux' ESCAPE '\'
  LOOP
    BEGIN
      EXECUTE 'ALTER EXTENSION accelerator_codegen_postgres DROP FUNCTION ' || fn.signature;
    EXCEPTION WHEN undefined_object THEN
      NULL;
    END;
    EXECUTE 'DROP FUNCTION IF EXISTS ' || fn.signature || ' CASCADE';
  END LOOP;
END
$drop$;
SQL
  local aux_sql
  aux_sql="$(mktemp)"
  {
    echo "DROP TYPE IF EXISTS accel_tpch_q6_row CASCADE;"
    echo "DROP TYPE IF EXISTS accel_tpch_q14_row CASCADE;"
    echo "DROP TYPE IF EXISTS accel_tpch_q17_row CASCADE;"
    sed "s|MODULE_PATHNAME|${EXTENSION_SO}|g" "$EXTENSION_SQL" | python3 -c '
import re, sys
text = sys.stdin.read()
for pattern in (
    r"CREATE TYPE accel_tpch_q6_row AS \([^;]+\);",
    r"CREATE TYPE accel_tpch_q14_row AS \([^;]+\);",
    r"CREATE TYPE accel_tpch_q17_row AS \([^;]+\);",
):
    for match in re.finditer(pattern, text, flags=re.S):
        print(match.group(0))
        print()
for match in re.finditer(
    r"CREATE FUNCTION accel_\w+_aux\([^)]*\)\s+RETURNS.*?^LANGUAGE C;",
    text,
    flags=re.S | re.M,
):
    print(match.group(0))
    print()
'
  } >"$aux_sql"
  psql_cmd -d "$db" -f "$aux_sql"
  rm -f "$aux_sql"
}

if ! pg_ready; then
  echo "PostgreSQL is not reachable at ${PG_HOST}:${PG_PORT}" >&2
  echo "start: docker start accel_pg_test" >&2
  exit 1
fi

if [[ ! -f "$DUCKDB" ]]; then
  echo "missing SF20 DuckDB: ${DUCKDB}" >&2
  echo "run: bash scripts/setup_tpch_sf20_db.sh" >&2
  exit 1
fi

free_gb="$(df -BG / | awk 'NR==2 {gsub(/G/,"",$4); print $4}')"
echo "free_disk_gb=${free_gb} duckdb=${DUCKDB} db=${DB}"
if [[ "${free_gb}" -lt "${MIN_DISK_GB}" ]]; then
  echo "ERROR: need >= ${MIN_DISK_GB} GB free disk for SF20 Postgres load, have ${free_gb} GB" >&2
  exit 1
fi

# Prefer the .so path inside accel_pg_test when the container is up.
if docker ps --format '{{.Names}}' 2>/dev/null | grep -qx 'accel_pg_test'; then
  if docker exec accel_pg_test test -f /usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so 2>/dev/null; then
    EXTENSION_SO="/usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so"
  fi
fi

db_exists="$(psql_cmd -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname = '${DB}'" || true)"
if [[ "$db_exists" == "1" && "$FORCE" != "1" && "$SKIP_LOAD" != "1" ]]; then
  echo "database ${DB} already exists (set FORCE=1 to recreate, or SKIP_LOAD=1 to only refresh extension)"
  psql_cmd -d "$DB" -c "SELECT 'lineitem' AS table, COUNT(*) FROM lineitem;"
elif [[ "$SKIP_LOAD" != "1" ]]; then
  echo "=== recreating PostgreSQL database: ${DB} ==="
  psql_cmd -d postgres -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '${DB}' AND pid <> pg_backend_pid();" >/dev/null || true
  psql_cmd -d postgres -c "DROP DATABASE IF EXISTS \"${DB}\";"
  psql_cmd -d postgres -c "CREATE DATABASE \"${DB}\";"

  echo "=== creating TPC-H schema ==="
  psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_tpch_schema.sql"

  echo "=== streaming SF20 load from DuckDB (can take 30-90+ min) ==="
  load_args=(--duckdb "$DUCKDB" --db "$DB")
  if [[ -n "$STAGING_DIR" ]]; then
    mkdir -p "$STAGING_DIR"
    load_args+=(--staging-dir "$STAGING_DIR")
  fi
  PG_HOST="$PG_HOST" PG_PORT="$PG_PORT" PGPASSWORD="$PGPASSWORD" \
    "$PYTHON" "${ROOT}/scripts/load_tpch_postgres_from_duckdb.py" "${load_args[@]}"

  echo "=== analyzing tables ==="
  psql_cmd -d "$DB" -c "ANALYZE;"
fi

echo "=== ensuring TPC-H indexes (required for SF20 base Q21 etc.) ==="
psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_tpch_indexes.sql"

echo "=== installing extension SQL on ${DB} ==="
sed "s|MODULE_PATHNAME|${EXTENSION_SO}|g" "$EXTENSION_SQL" \
  | psql_cmd -d "$DB" -v ON_ERROR_STOP=0 -f - 2>&1 | tail -20 || true

reinstall_aux_functions "$DB"

echo "=== creating mutation capture triggers ==="
psql_cmd -d "$DB" -f "${ROOT}/scripts/postgres_accel_mutation_triggers.sql"

echo "=== SF20 Postgres database ready: ${DB} ==="
psql_cmd -d "$DB" -c "
SELECT 'lineitem' AS table, COUNT(*)::bigint AS rows FROM lineitem
UNION ALL SELECT 'orders', COUNT(*) FROM orders
UNION ALL SELECT 'partsupp', COUNT(*) FROM partsupp
UNION ALL SELECT 'part', COUNT(*) FROM part
ORDER BY 1;"
psql_cmd -d "$DB" -c "SELECT proname FROM pg_proc WHERE proname LIKE 'accel_tpch_q%_aux' ORDER BY 1;"
psql_cmd -d "$DB" -tAc "SELECT 'triggers=' || COUNT(*) FROM pg_trigger WHERE NOT tgisinternal;"
