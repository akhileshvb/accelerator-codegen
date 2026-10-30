#!/usr/bin/env bash
# SF20 read-only: stateful C++ accelerators vs DuckDB, 40 vs 40 threads, socket 0, PK database.
# Produces data/cpp_read (DuckDB reference base_us + accel_us per case). Original run: commit 29ed49f7.
# Env: ACCEL_DUCKDB_ROOT, DB, WORK, OUT; optional REPO, CORES.
set -uo pipefail
ulimit -c 0
REPO=${REPO:-$(git rev-parse --show-toplevel)}
CORES=${CORES:-0-19,40-59}
export ACCEL_DUCKDB_ROOT=${ACCEL_DUCKDB_ROOT:?ACCEL_DUCKDB_ROOT: root of the DuckDB capture fork}
DB=${DB:?DB: tpch_sf20_pk.duckdb (see make_pk_db.sh)}; WORK=${WORK:?WORK: scratch dir for per-case files}; O=${OUT:?OUT: output dir}
cd $REPO
DB=$DB WORK=$WORK MAX_LOAD=2 numactl --membind=0 scripts/perf/run_sf20_read.sh $REPO $CORES $O/cpp_read --seed 632608726
