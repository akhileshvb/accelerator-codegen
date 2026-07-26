#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ACCEL_BUILD_DIR:-$ROOT/build}"
DB_PATH="${ACCEL_TPCH_DB:-$ROOT/../accelerator-codegen-duckdb/tpch_sf1.duckdb}"
OUT_PATH="${ACCEL_Q14_COST_MODEL_PATH:-$ROOT/config/q14_aux_cost_model.json}"

cmake --build "$BUILD_DIR" --target calibrate_q14_aux_cost_model

"$BUILD_DIR/calibrate_q14_aux_cost_model" "$DB_PATH" "$OUT_PATH"
echo "Wrote calibration to $OUT_PATH"
