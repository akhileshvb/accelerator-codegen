#!/usr/bin/env bash
# Creates the SEC-EDGAR Postgres container (postgres:17, loopback port 5461) and loads the SEC-EDGAR DuckDB database into it:
# workloads/sec_edgar/schema.sql, the tables as CSV through psql COPY, scripts/postgres_sec_edgar_setup.sql (C collation,
# indexes, ANALYZE), then row counts compared with the DuckDB file.  The accelerator extension is built and installed
# separately (../pg_sf20/build_ext.sh, ../pg_sf20/install_ext.sh with REGISTER=register.sql here).
# The container sets no max_parallel_workers[_per_gather] on its command line: that would beat ALTER SYSTEM (../pg_sf20/set_gucs.sh).
# EXISTING=1: the container is already up (e.g. started by backends/postgres/host.sh up with the same server settings) and
# only the database is created.  TEXT_C=1: every text column is COLLATE "C" (the Verus kits require it of their captured columns).
# Env: T (container, accel_pg_sec_edgar), PG_PORT (5461), DB (sec_edgar), DATA (host dir of the data directory and the CSV
# scratch, /flash1/akhilvb/pg_sec_edgar), SRC (the DuckDB file, /flash1/akhilvb/sec_edgar_raw/sec_edgar_sf3.duckdb),
# DUCKDB (the DuckDB CLI).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
T=${T:-accel_pg_sec_edgar}; PG_PORT=${PG_PORT:-5461}; DB=${DB:-sec_edgar}; DATA=${DATA:-/flash1/akhilvb/pg_sec_edgar}
SRC=${SRC:-/flash1/akhilvb/sec_edgar_raw/sec_edgar_sf3.duckdb}; DUCKDB=${DUCKDB:-/home/akhilvb/duckdb-dbcapture/build/release/duckdb}
TABLES=(sub tag num pre)
psql_db() { docker exec -i "$T" psql -U postgres -d "$DB" -v ON_ERROR_STOP=1 "$@"; }

mkdir -p "$DATA/csv"
[ "${EXISTING:-0}" = 1 ] || docker run -d --name "$T" --shm-size 16g -p "127.0.0.1:$PG_PORT:5432" -e POSTGRES_PASSWORD=postgres \
  -v "$DATA/data:/var/lib/postgresql/data" postgres:17 \
  -c shared_buffers=16GB -c work_mem=64MB -c maintenance_work_mem=2GB -c max_wal_size=8GB -c autovacuum=off \
  -c max_worker_processes=64 >/dev/null
until docker exec "$T" pg_isready -U postgres >/dev/null 2>&1; do sleep 1; done
docker exec "$T" psql -U postgres -v ON_ERROR_STOP=1 -c "CREATE DATABASE $DB" >/dev/null 2>&1 || [ "${EXISTING:-0}" = 1 ]
TEXT_COLLATION=(-e '')
[ "${TEXT_C:-0}" = 1 ] && TEXT_COLLATION=(-e 's/\(VARCHAR([0-9]*)\)/\1 COLLATE "C"/' -e 's/\bTEXT\b/TEXT COLLATE "C"/')
sed -e 's/\bDOUBLE\b/double precision/' "${TEXT_COLLATION[@]}" "$ROOT/workloads/sec_edgar/schema.sql" | psql_db -q  # the schema is DuckDB's

for t in "${TABLES[@]}"; do
  "$DUCKDB" -readonly "$SRC" -c "COPY $t TO '$DATA/csv/$t.csv' (FORMAT csv, HEADER false)"
  psql_db -q -c "COPY $t FROM STDIN WITH (FORMAT csv)" < "$DATA/csv/$t.csv"
  rm -f "$DATA/csv/$t.csv"
done
psql_db -q < "$ROOT/scripts/postgres_sec_edgar_setup.sql"

for t in "${TABLES[@]}"; do
  want=$("$DUCKDB" -readonly "$SRC" -csv -noheader -c "SELECT count(*) FROM $t")
  got=$(psql_db -Atq -c "SELECT count(*) FROM $t")
  echo "$t duckdb=$want postgres=$got"
  [ "$want" = "$got" ] || { echo "row count mismatch in $t" >&2; exit 1; }
done
