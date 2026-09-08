#!/usr/bin/env bash
# Prepare Postgres databases for aux performance sweeps (TPC-H + SEC SF3).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_HOST="${PG_HOST:-127.0.0.1}"
PG_PORT="${PG_PORT:-5434}"
PG_TPCH_DB="${PG_TPCH_DB:-accel_aux_bench_q10}"
PG_SEC_DB="${PG_SEC_DB:-accel_sec_edgar_sf3}"
SEC_DUCKDB="${SEC_EDGAR_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
EXTENSION_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so}"
EXTENSION_SQL="${ROOT}/src/runtime/postgres/accelerator_codegen_postgres--0.1.sql"
FORCE_SEC_RELOAD="${FORCE_SEC_RELOAD:-0}"

export PGHOST="$PG_HOST"
export PGPORT="$PG_PORT"
export PGUSER="${PG_USER:-postgres}"
export PGPASSWORD="${PGPASSWORD:-postgres}"

psql_cmd() {
  psql -v ON_ERROR_STOP=1 "$@"
}

pg_ready() {
  psql_cmd -d postgres -tAc 'select 1' >/dev/null 2>&1
}

pg_db_has_fn() {
  local db="$1"
  local fn="$2"
  psql_cmd -d "$db" -tAc "SELECT 1 FROM pg_proc WHERE proname = '${fn}'" 2>/dev/null | grep -q 1
}

install_extension_sql() {
  local db="$1"
  echo "=== installing extension SQL on ${db} ==="
  sed "s|MODULE_PATHNAME|${EXTENSION_SO}|g" "$EXTENSION_SQL" | psql_cmd -d "$db" -v ON_ERROR_STOP=0 -f - 2>&1 | tail -10 || true
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
  local aux_sql="${TMPDIR:-/tmp}/accel_aux_functions_$$.sql"
  cat >"$aux_sql" <<'SQL'
DROP TYPE IF EXISTS accel_tpch_q6_row CASCADE;
DROP TYPE IF EXISTS accel_tpch_q14_row CASCADE;
DROP TYPE IF EXISTS accel_tpch_q17_row CASCADE;
SQL
  sed "s|MODULE_PATHNAME|${EXTENSION_SO}|g" "$EXTENSION_SQL" | python3 -c '
import re, sys
text = sys.stdin.read()
for match in re.finditer(
    r"CREATE TYPE accel_tpch_q\d+_row AS \([^;]+\);",
    text,
    flags=re.S,
):
    stmt = match.group(0).rstrip(";")
    print("DO $$ BEGIN")
    print("  " + stmt + ";")
    print("EXCEPTION WHEN duplicate_object THEN NULL;")
    print("END $$;")
    print()
for match in re.finditer(
    r"CREATE FUNCTION accel_\w+_aux\([^)]*\)\s+RETURNS.*?^LANGUAGE C;",
    text,
    flags=re.S | re.M,
):
    print(match.group(0))
    print()
' >>"$aux_sql"
  psql_cmd -d "$db" -f "$aux_sql"
  rm -f "$aux_sql"
  psql_cmd -d "$db" <<'SQL'
DO $add$
DECLARE
  fn record;
BEGIN
  IF EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'accelerator_codegen_postgres') THEN
    FOR fn IN
      SELECT p.oid::regprocedure AS signature
      FROM pg_proc p
      WHERE p.proname LIKE 'accel\_%\_aux' ESCAPE '\'
    LOOP
      BEGIN
        EXECUTE 'ALTER EXTENSION accelerator_codegen_postgres ADD FUNCTION ' || fn.signature;
      EXCEPTION WHEN duplicate_object THEN
        NULL;
      END;
    END LOOP;
  END IF;
END
$add$;
SQL
}

echo "=== syncing extension source into docker and rebuilding ==="
if [[ "${SKIP_EXTENSION_REBUILD:-0}" == "1" ]]; then
  echo "SKIP_EXTENSION_REBUILD=1: using existing ${EXTENSION_SO}"
elif docker ps --format '{{.Names}}' | grep -qx 'accel_pg_test'; then
  docker cp "${ROOT}/src/." accel_pg_test:/tmp/accel-codegen-src/src/
  docker cp "${ROOT}/include/accelerator_codegen/." \
    accel_pg_test:/tmp/accel-codegen-src/include/accelerator_codegen/
  docker cp "${ROOT}/config/." accel_pg_test:/tmp/accel-codegen-src/config/
  docker exec accel_pg_test bash -c '
    set -e
    cd /tmp/accel-codegen-src/build
    cmake --build . --target accelerator_codegen_postgres -j"$(nproc)"
    cp -f accelerator_codegen_postgres.so /usr/lib/postgresql/17/lib/
  '
  EXTENSION_SO="/usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so"
else
  echo "warning: accel_pg_test container not running; using EXTENSION_SO=${EXTENSION_SO}" >&2
fi

if ! pg_ready; then
  echo "PostgreSQL is not reachable at ${PG_HOST}:${PG_PORT}" >&2
  exit 1
fi

echo "=== TPC-H performance database: ${PG_TPCH_DB} ==="
reinstall_aux_functions "$PG_TPCH_DB"
bash "${ROOT}/scripts/ensure_accel_postgres_mutation_triggers.sh" "$PG_TPCH_DB"

if [[ "${SKIP_SEC:-0}" == "1" ]]; then
  echo "=== skipping SEC EDGAR SF3 (SKIP_SEC=1) ==="
else
echo "=== SEC EDGAR SF3 performance database: ${PG_SEC_DB} ==="
if [[ "$FORCE_SEC_RELOAD" == "1" ]] || ! psql_cmd -d postgres -tAc \
    "SELECT 1 FROM pg_database WHERE datname = '${PG_SEC_DB}'" | grep -q 1; then
  ACCEL_POSTGRES_SO="$EXTENSION_SO" SEC_PG_DB="$PG_SEC_DB" SEC_EDGAR_DB="$SEC_DUCKDB" \
    bash "${ROOT}/scripts/setup_accel_postgres_sec_sf3_db.sh"
else
  reinstall_aux_functions "$PG_SEC_DB"
  bash "${ROOT}/scripts/ensure_accel_postgres_mutation_triggers.sh" "$PG_SEC_DB"
fi
fi

echo "=== smoke checks ==="
psql_cmd -d "$PG_TPCH_DB" -c "SELECT 'tpch lineitem' AS what, COUNT(*) FROM lineitem;"
psql_cmd -d "$PG_TPCH_DB" -c "SELECT proname FROM pg_proc WHERE proname LIKE 'accel_tpch_q%_aux' ORDER BY 1;"
psql_cmd -d "$PG_TPCH_DB" -tAc "SELECT COUNT(*) FROM pg_trigger WHERE NOT tgisinternal;"
if [[ "${SKIP_SEC:-0}" != "1" ]]; then
  psql_cmd -d "$PG_SEC_DB" -c "SELECT 'sec num' AS what, COUNT(*) FROM num;"
  psql_cmd -d "$PG_SEC_DB" -c "SELECT proname FROM pg_proc WHERE proname LIKE 'accel_sec_edgar%_aux' ORDER BY 1;"
  psql_cmd -d "$PG_SEC_DB" -tAc "SELECT COUNT(*) FROM pg_trigger WHERE NOT tgisinternal;"
fi

echo "=== Postgres aux performance databases ready ==="
