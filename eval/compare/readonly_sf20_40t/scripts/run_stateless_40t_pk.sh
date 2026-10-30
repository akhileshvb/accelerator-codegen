#!/usr/bin/env bash
# SF20 stateless C++ vs DuckDB, 40 vs 40 threads only (no 1-thread pair, no correctness pass), socket 0.
set -euo pipefail
ulimit -c 0
REPO=/spinning/akhilvb/accel_sf20/accelerator-codegen
CORES=0-19,40-59; T=40
export ACCEL_DUCKDB_ROOT=/spinning/akhilvb/accel_sf20/accelerator-codegen-duckdb TMPDIR=/spinning/akhilvb/accel_sf20/tmp
export ACCEL_TPCH_DB=/spinning/akhilvb/accel_sf20/tpch_sf20_pk.duckdb
O=/spinning/akhilvb/accel_sf20/results_40t_socket0_pk/stateless
cd $REPO
echo "$(date +%T) build"
cmake -S . -B build-stateless -DCMAKE_BUILD_TYPE=Release -DACCEL_CODEGEN_ENGINE=duckdb -DACCEL_ENABLE_PARALLEL_KERNELS=ON >/dev/null
cmake --build build-stateless -j"$(nproc)" --target stateless_matrix >/dev/null
while l=$(cut -d' ' -f1 /proc/loadavg); ! awk -v l="$l" 'BEGIN{exit !(l < 2)}'; do echo "$(date +%T) waiting for quiet machine (load $l)"; sleep 30; done
OUT=$O/stateless_duckdb_sweep_t${T}only_sf20_$(date +%Y%m%d_%H%M%S).csv
echo "$(date +%T) sweep T=$T seeds=20 -> $OUT"
numactl --membind=0 taskset -c $CORES build-stateless/stateless_matrix --mode sweep --queries $(seq -s, 1 22) \
  --scale-factor 20 --seeds 20 --threads-list $T --skip-st 1 > "$OUT"
grep '^#' "$OUT"
echo "$(date +%T) ALL DONE raw: $OUT"
