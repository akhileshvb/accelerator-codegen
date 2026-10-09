#!/usr/bin/env bash
# The stateless kernels (accel_tpch_qN, single-threaded), all 22 in one process, under the T=60s (~2.5k rows/s) TPC-H
# refresh stream of rf_interval_multi_sf20_20261008_112014/T60s: same seed and parameter draws, readers, duration,
# DuckDB threads=1 and memory as that run's DuckDB / C++ / Verus runs, so workload.py pairs them by repetition
# (results land beside them as rep<k>/multi_stateless.json). Waits for the running batch sweep (it launches
# build-verus-duckdb/aux_compare) before rebuilding aux_compare.
#
#   benchmarks/compare/run_stateless_T60.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
R=benchmarks/compare/results
export WORK=/flash1/akhilvb/tpch/work_stateless
export ACCEL_COMPARE_DUCKDB_THREADS=1  # as run_rf_interval_sweep.sh
mkdir -p "$WORK"

while [[ ! -f $R/writerate_batch100_20261009_165045/ALL_DONE ]]; do sleep 60; done
echo "start $(date)"
cmake --build build-verus-duckdb --target aux_compare -j 16

python3 benchmarks/compare/run_compare.py multi --db ../accelerator-codegen-duckdb/tpch_sf20.duckdb --sf 20 \
  --rf-dir /flash1/akhilvb/tpch/refresh_sf20 --dim-after-rf --systems stateless --no-base \
  --readers 8 --seconds 240 --wps 0.016666666666666666 --sets 8 --repeats 3 --seed 267713173 --duckdb-memory 16GB \
  --out $R/rf_interval_multi_sf20_20261008_112014/T60s
echo "=== ALL DONE $(date)"
