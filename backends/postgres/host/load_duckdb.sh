#!/usr/bin/env bash
# Load a DuckDB database into the instance's Postgres database: its tables' schema (types, no
# constraints: the test databases hold duplicate keys on purpose) through schema_to_pg.sed (a bare VARCHAR gets a placeholder length, which the sed drops), its rows
# through COPY ... csv.  An unquoted empty field is NULL, "" the empty string, on both sides.
#   ACCEL_ENGINE_INSTANCE=... ACCEL_ENGINE_STATE_DIR=... load_duckdb.sh <database.duckdb> [constraints.sql]
# Env: DUCKDB_CLI (default: the dbcapture checkout's build), TMPDIR (scratch for the csv files).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${ACCEL_ENGINE_INSTANCE:?}" "${ACCEL_ENGINE_STATE_DIR:?}"
DB_FILE="$1" CONSTRAINTS="${2:-}"
NAME="accel_pg_${ACCEL_ENGINE_INSTANCE}"
DB="accel_${ACCEL_ENGINE_INSTANCE//[^a-zA-Z0-9]/_}"
DUCKDB_CLI="${DUCKDB_CLI:-${HOME}/duckdb-dbcapture/build/release/duckdb}"
SCRATCH="$(mktemp -d "${TMPDIR:-/dev/shm}/load_duckdb.XXXXXX")"
trap 'rm -rf "${SCRATCH}"' EXIT
psql_in() { docker exec -i -e PGOPTIONS="-c client_min_messages=warning" "${NAME}" psql -X -q -v ON_ERROR_STOP=1 -U postgres -d "${DB}" "$@"; }

psql_in -c 'DROP SCHEMA public CASCADE; CREATE SCHEMA public'
"${DUCKDB_CLI}" -readonly "${DB_FILE}" -noheader -list -c "SELECT sql FROM duckdb_tables() ORDER BY table_name" |
  sed -E 's/\bVARCHAR(( PRIMARY KEY)?[,)])/VARCHAR(1)\1/g' | sed -E -f "${HERE}/schema_to_pg.sed" | psql_in -f -
for table in $("${DUCKDB_CLI}" -readonly "${DB_FILE}" -noheader -list -c "SELECT table_name FROM duckdb_tables()"); do
  "${DUCKDB_CLI}" -readonly "${DB_FILE}" -c "COPY \"${table}\" TO '${SCRATCH}/${table}.csv' (FORMAT csv, DELIMITER '|', HEADER false)"
  psql_in -c "COPY \"${table}\" FROM STDIN WITH (FORMAT csv, DELIMITER '|', NULL '')" <"${SCRATCH}/${table}.csv"
done
[[ -z "${CONSTRAINTS}" ]] || psql_in -f - <"${CONSTRAINTS}"
psql_in -c 'ANALYZE'
