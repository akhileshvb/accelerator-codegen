#!/usr/bin/env bash
# TPC-H Q1 at SF20: 620 queries in four write phases (TRACE_SCHEDULE=contrast), each under the cost model,
# always ApplyDelta, always RebuildAll, and plain DuckDB; TRACE_ROUNDS rounds rotate which strategy runs first.
# Writes data/q1_trace_contrast_sf20.csv. Needs a DuckDB build (see the top-level README) with the
# maintenance_choice_demo target.
set -euo pipefail
REPO=${REPO:-$(git rev-parse --show-toplevel)}
BUILD=${BUILD:-$REPO/build}
DB=${DB:?set DB to a TPC-H SF20 DuckDB database (it is copied, not modified)}
WORK=${WORK:-${TMPDIR:-/tmp}/q1_maintenance}
OUT=${OUT:-$(dirname "$0")/../data/q1_trace_contrast_sf20.csv}

cmake --build "$BUILD" --target maintenance_choice_demo
mkdir -p "$WORK"
cp "$DB" "$WORK/accel.duckdb"
cp "$DB" "$WORK/base.duckdb"
ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL=${COST_MODEL:-$REPO/accelerators/tpch/q1/config/q1_aux_cost_model_sf20.json} \
TRACE_SCHEDULE=contrast TRACE_ROUNDS=${TRACE_ROUNDS:-3} \
  "$BUILD/maintenance_choice_demo" "$WORK/accel.duckdb" 1 trace "$WORK/base.duckdb" "$OUT"
