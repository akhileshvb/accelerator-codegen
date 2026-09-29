#!/usr/bin/env bash
# (Re)create the small accel_q21_rt database used by the Q21 live runtime
# gates: TPC-H schema, accelerator SQL functions, capture triggers (incl.
# TRUNCATE).  Needs the accel_pg_test container with the extension installed
# (scripts/ensure_accel_pg_test.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
export PGHOST="${PGHOST:-127.0.0.1}" PGPORT="${PGPORT:-5434}" PGUSER="${PGUSER:-postgres}"
export PGPASSWORD="${PGPASSWORD:-postgres}"
DB="${Q21_RT_DB:-accel_q21_rt}"
SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so}"
psql -v ON_ERROR_STOP=1 -q -d postgres \
  -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '${DB}' AND pid <> pg_backend_pid();" >/dev/null
psql -v ON_ERROR_STOP=1 -q -d postgres -c "DROP DATABASE IF EXISTS \"${DB}\"" -c "CREATE DATABASE \"${DB}\""
psql -v ON_ERROR_STOP=1 -q -d "$DB" -f "$ROOT/scripts/postgres_tpch_schema.sql"
psql -v ON_ERROR_STOP=1 -q -d "$DB" \
  -c "INSERT INTO region VALUES (0,'R','c')" \
  -c "INSERT INTO nation VALUES (0,'SAUDI ARABIA',0,'c'),(1,'PERU',0,'c')" \
  -c "INSERT INTO supplier VALUES (1,'Supp#1','a',0,'p',0,'c'),(2,'Supp#2','a',0,'p',0,'c'),(3,'Supp#3','a',1,'p',0,'c')"
psql -v ON_ERROR_STOP=1 -q -d "$DB" -c "LOAD '${SO}'" \
  -f "$ROOT/scripts/postgres_register_tpch_all.sql" 2>&1 | grep -v NOTICE || true
psql -v ON_ERROR_STOP=1 -q -d "$DB" -f "$ROOT/scripts/postgres_accel_mutation_triggers.sql" >/dev/null 2>&1
n=$(psql -tA -d "$DB" -c "SELECT count(*) FROM pg_trigger WHERE tgname LIKE 'accel_codegen_tpch_%_t'")
[[ "$n" -ge 8 ]] || { echo "TRUNCATE capture triggers missing ($n)" >&2; exit 1; }
echo "accel_q21_rt ready"
