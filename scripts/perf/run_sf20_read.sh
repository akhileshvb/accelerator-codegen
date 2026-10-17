#!/usr/bin/env bash
# Read-only sweep: plain DuckDB vs the C++ accelerators, per TPC-H query, median [min-max] over fresh random parameter draws.
#   scripts/perf/run_sf20_read.sh <tree> <cores> <out> [extra run_compare.py read args]
# Env: SF (20), DB (accelerator-codegen-duckdb/tpch_sf<SF>.duckdb), REPEATS (5), SETS (8 parameter sets per draw), QUERIES (1..22).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[ $# -ge 3 ] || { sed -n 2,5p "$0"; exit 2; }
perf_require_quiet
perf_setup "$1" "$2"; OUT=$3; shift 3
SF=${SF:-20}
DB=${DB:-$(dirname "$PERF_ROOT")/accelerator-codegen-duckdb/tpch_sf${SF}.duckdb}
mkdir -p "$OUT"
perf_record_settings "$OUT"
# --copy: each (query, system) process works on a fresh copy, the original is never opened for writing
$RUN read --db "$DB" --sf "$SF" --systems cpp --copy --out "$OUT" --bin "$TREE/build-release/aux_compare" \
  --repeats "${REPEATS:-5}" --sets "${SETS:-8}" --queries "${QUERIES:-$(seq -s' ' 1 22)}" $SQLDIR_FLAG "$@" 2>&1 | tee "$OUT/run.log"
