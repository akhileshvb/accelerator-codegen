#!/usr/bin/env bash
# Start accel_pg40 (postgres:17, 127.0.0.1:5470) with its data dir on tmpfs and load official-dbgen TPC-H SF20
# (/spinning/akhilvb/gendb/sf20_dbgen) with COPY, then the repo's PK/indexes and ANALYZE.
set -euo pipefail
REPO=/spinning/akhilvb/accel_sf20/accelerator-codegen
TBL=/spinning/akhilvb/gendb/sf20_dbgen
DATA=${DATA:-/tmp/pg40/data}
NAME=accel_pg40; PORT=5470; DB=accel_tpch_sf20
export PGPASSWORD=postgres
PSQL="psql -h 127.0.0.1 -p $PORT -U postgres -v ON_ERROR_STOP=1"

mkdir -p "$DATA"
docker rm -f $NAME >/dev/null 2>&1 || true
docker run -d --name $NAME --shm-size=48g -e POSTGRES_PASSWORD=postgres -p 127.0.0.1:$PORT:5432 \
  -v "$DATA":/var/lib/postgresql/data -v "$REPO":/accel-src:ro -v "$TBL":/tbl:ro \
  -v /spinning/akhilvb/pg40/refresh_sf20:/refresh:ro \
  postgres:17 \
  -c shared_buffers=32GB -c work_mem=256MB -c maintenance_work_mem=8GB -c max_wal_size=64GB \
  -c checkpoint_timeout=30min -c autovacuum=off -c wal_level=minimal -c max_wal_senders=0 \
  -c synchronous_commit=off -c max_parallel_maintenance_workers=16 -c max_worker_processes=128 \
  -c max_parallel_workers=40 >/dev/null
for _ in $(seq 120); do $PSQL -d postgres -tAc 'select 1' >/dev/null 2>&1 && break; sleep 1; done
$PSQL -d postgres -c "DROP DATABASE IF EXISTS $DB" -c "CREATE DATABASE $DB"
$PSQL -d $DB -f /dev/stdin < "$REPO/scripts/postgres_tpch_schema.sql"

load() {  # dbgen rows end in '|': strip it
  local t=$1 s=$(date +%s)
  $PSQL -d $DB -c "COPY $t FROM PROGRAM 'sed ''s/|\$//'' /tbl/$t.tbl' WITH (FORMAT text, DELIMITER '|')"
  echo "loaded $t in $(( $(date +%s) - s ))s"
}
for t in region nation supplier customer part partsupp orders; do load $t & done
load lineitem & wait

s=$(date +%s)
# one statement per index, run concurrently (each table's PK first)
mapfile -t stmts < <(grep -E '^(ALTER|CREATE)' "$REPO/scripts/postgres_tpch_indexes.sql")
for st in "${stmts[@]}"; do [[ $st == ALTER* ]] && $PSQL -d $DB -c "$st" & done; wait
for st in "${stmts[@]}"; do [[ $st == CREATE* ]] && $PSQL -d $DB -c "$st" & done; wait
$PSQL -d $DB -c "VACUUM (FREEZE, ANALYZE)"
echo "indexes+analyze in $(( $(date +%s) - s ))s"
$PSQL -d $DB -c "SELECT relname, n_live_tup FROM pg_stat_user_tables ORDER BY 1" \
  -c "SELECT pg_size_pretty(pg_database_size('$DB'))"
echo LOAD_DONE
