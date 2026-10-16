#!/usr/bin/env bash
# TPC-H performance test (power + throughput, QphH@Size): DuckDB vs the C++ accelerators, on a fresh copy of the database.
#   scripts/perf/run_qphh.sh <tree> <sf> <cores> <out> [extra run_compare.py tpch args]
# Env: DB (accelerator-codegen-duckdb/tpch_sf<SF>.duckdb), RF_DIR (/flash1/akhilvb/tpch/refresh_sf<SF>: dbgen -U update sets,
#      scripts/gen_tpch_refresh_sets.sh), RUNS (3), STREAMS (3), WORK (copy goes here: ~5 GB at SF20, deleted after each system).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[ $# -ge 4 ] || { sed -n 2,5p "$0"; exit 2; }
perf_require_quiet
perf_setup "$1" "$3"; SF=$2; OUT=$4; shift 4
DB=${DB:-$(dirname "$PERF_ROOT")/accelerator-codegen-duckdb/tpch_sf${SF}.duckdb}
mkdir -p "$OUT" "$WORK"
need=$(( $(stat -c %s "$DB") / 1048576 * 2 ))  # MB: the copy and its WAL/growth
avail=$(df -Pm "$WORK" | awk 'NR==2{print $4}')
[ "$avail" -gt "$need" ] || { echo "$WORK has ${avail} MB free, need ~${need}" >&2; exit 1; }
$RUN tpch --db "$DB" --sf "$SF" --systems cpp --out "$OUT" --bin "$TREE/build-release/aux_compare" \
  --rf-dir "${RF_DIR:-/flash1/akhilvb/tpch/refresh_sf${SF}}" --runs "${RUNS:-3}" --streams "${STREAMS:-3}" \
  --duckdb-threads "$N" $SQLDIR_FLAG "$@" 2>&1 | tee "$OUT/run.log"
