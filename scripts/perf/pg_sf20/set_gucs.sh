#!/usr/bin/env bash
# Thread GUCs for an N-core sweep (every cap = N, maintenance included; default 8 = CPUs 32-39), then restart the server and print the settings.
# Env: N (8), T (container), PG_PORT, DB. Note: the container's `-c max_parallel_workers=48` command-line setting beats
# ALTER SYSTEM, so sweep.py also runs SET max_parallel_workers = 8 in every session.
set -euo pipefail
N=${N:-8}; T=${T:-accel_pg_sf20}
P="psql -h 127.0.0.1 -p ${PG_PORT:-5460} -U postgres -d ${DB:-accel_tpch_sf20} -Atq"; export PGPASSWORD=postgres
$P -c "ALTER SYSTEM SET max_parallel_workers_per_gather=$N" -c "ALTER SYSTEM SET max_parallel_workers=$N" \
   -c "ALTER SYSTEM SET accel.threads=$N" -c "ALTER SYSTEM SET accel.read_threads=$N" -c "ALTER SYSTEM SET accel.build_threads=$N" \
   -c "ALTER SYSTEM SET accel.maint_threads=$N" -c "ALTER SYSTEM SET accel.shard_read_threads=$N"
docker restart $T >/dev/null
until $P -c 'select 1' >/dev/null 2>&1; do sleep 1; done
$P -c "LOAD 'accelerator_codegen_pg'" -c "SELECT name||'='||setting FROM pg_settings WHERE name IN ('max_parallel_workers_per_gather','max_parallel_workers','jit','shared_buffers','work_mem','accel.threads','accel.read_threads','accel.build_threads','accel.maint_threads','accel.shard_read_threads','accel.shard_idle_exit_s') ORDER BY 1"
