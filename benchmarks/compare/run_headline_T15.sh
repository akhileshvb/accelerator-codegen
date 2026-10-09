#!/usr/bin/env bash
# The headline refresh-stream panel (plot_headline_speedup.py, --interval 15): Verus (verus_global) and the stateless
# kernels in the concurrent RF-interval sweep at T=15s (~10k rows/s), paired with the DuckDB and C++ runs of
# rf_interval_sf20_20261007_222743/T15s: same seed (one parameter draw), refresh sets, 8 single-threaded readers,
# DuckDB threads=1 and 60 s runs; --no-base, so the old DuckDB runs are the baseline.  Waits for the running batch
# sweep (it launches build-verus-duckdb/aux_compare) before rebuilding aux_compare.
#
#   benchmarks/compare/run_headline_T15.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
R=benchmarks/compare/results
OUT=$R/rf_interval_sf20_T15_headline
export ACCEL_COMPARE_DUCKDB_THREADS=1  # as run_rf_interval_sweep.sh

while [[ ! -f $R/writerate_batch100_20261009_165045/ALL_DONE ]]; do sleep 60; done
echo "start $(date)"
cmake --build build-verus-duckdb --target aux_compare -j 16
mkdir -p "$OUT"

systemd-run --user --scope -p MemoryMax=64G -p MemorySwapMax=0 \
  python3 benchmarks/compare/run_compare.py concurrent --db ../accelerator-codegen-duckdb/tpch_sf20.duckdb --sf 20 \
  --work /flash1/akhilvb/tpch/work --rf-dir /flash1/akhilvb/tpch/refresh_sf20 --dim-after-rf \
  --systems "verus stateless" --no-base --queries "$(seq -s ' ' 1 22)" \
  --readers 8 --seconds 60 --wps 0.06666666666666667 --sets 8 --repeats 1 --seed 694380215 \
  --out "$OUT/T15s" 2>&1 | tee "$OUT/T15s.log"
echo "=== ALL DONE $(date)" > "$OUT/ALL_DONE"
