#!/usr/bin/env bash
# SF20 read-only: stateful C++ accelerators vs DuckDB, 40 vs 40 threads, socket 0, PK database.
# Produces data/cpp_read (DuckDB reference base_us + accel_us per case). Original run: commit 29ed49f7.
set -uo pipefail
ulimit -c 0
REPO=/spinning/akhilvb/accel_sf20/accelerator-codegen
CORES=0-19,40-59
export ACCEL_DUCKDB_ROOT=/spinning/akhilvb/accel_sf20/accelerator-codegen-duckdb TMPDIR=/spinning/akhilvb/accel_sf20/tmp
DB=/spinning/akhilvb/accel_sf20/tpch_sf20_pk.duckdb; WORK=/ssd1/akhilvb/accel_sf20_work; O=/spinning/akhilvb/accel_sf20/results_40t_socket0_pk
cd $REPO
DB=$DB WORK=$WORK MAX_LOAD=2 numactl --membind=0 scripts/perf/run_sf20_read.sh $REPO $CORES $O/cpp_read --seed 632608726
