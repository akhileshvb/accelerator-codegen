#!/usr/bin/env bash
# Build and install the Postgres module (which contains the base accelerators)
# in the PostgreSQL container and (re)create the SQL functions
# accel_tpch_q<N>(...) / accel_tpch_q<N>_parallel(...) in a schema.
#
#   scripts/build_base_pg.sh [--schema SCHEMA] [--db DB] [--no-register]
#
# Defaults: schema public, db $TPCH_PG_DB or accel_tpch_test, container
# $ACCEL_PG_CONTAINER or accel_pg_test.  The module build is
# scripts/build_accel_postgres_extension.sh (SIMD flags: -DACCEL_BASE_COPTS).
# Use a separate --schema to keep the base functions next to other accelerator
# functions on the same database.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
SCHEMA=public
DB="${TPCH_PG_DB:-accel_tpch_test}"
REGISTER=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --schema) SCHEMA="$2"; shift 2 ;;
    --db) DB="$2"; shift 2 ;;
    --no-register) REGISTER=0; shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done

ACCEL_PG_CONTAINER="$CONTAINER" "$ROOT/scripts/build_accel_postgres_extension.sh"

if [[ "$REGISTER" == "1" ]]; then
  docker exec "$CONTAINER" psql -U "${PGUSER:-postgres}" -d "$DB" -v ON_ERROR_STOP=1 -qAt -c "
CREATE SCHEMA IF NOT EXISTS \"${SCHEMA}\";
DO \$\$ DECLARE r record; BEGIN
  -- Drop every overload of accel_tpch_qN / accel_tpch_qN_parallel in the schema (e.g. older
  -- definitions with other argument types) so calls resolve to the base kernels.
  FOR r IN SELECT p.oid::regprocedure AS f FROM pg_proc p
           WHERE p.pronamespace = '\"${SCHEMA}\"'::regnamespace
             AND p.proname ~ '^accel_tpch_q[0-9]+(_parallel)?\$' LOOP
    EXECUTE 'DROP FUNCTION ' || r.f || ' CASCADE';
  END LOOP;
END \$\$;
CREATE OR REPLACE FUNCTION \"${SCHEMA}\".accel_base_register(text) RETURNS integer
  AS '\$libdir/accelerator_codegen_pg', 'accel_base_register' LANGUAGE C;
SELECT 'registered ' || \"${SCHEMA}\".accel_base_register('${SCHEMA}') || ' functions in schema ${SCHEMA}';"
fi
