#!/usr/bin/env bash
# Read-only stateless TPC-H accelerators vs DuckDB (all 22 queries).
#
#   SF=20 scripts/run_stateless_duckdb.sh [threads-list] [seeds]      (defaults: 16,32 and 20)
#
# 1. builds the library + harness (build-stateless/, Release, parallel kernels on),
# 2. correctness: accel_tpch_qN and accel_tpch_qN_parallel vs the reference SQL on every
#    parametrization, at 1 thread and at each T in the threads list,
# 3. sweep: per query, every timed repetition is a different parametrization (one manifest
#    seed per rep): DuckDB and accel_tpch_qN at 1 thread, DuckDB and accel_tpch_qN_parallel
#    at T threads (PRAGMA threads=T for both).
#
# Env: SF (TPC-H scale factor, default 1; selects the database file and Q11's FRACTION = 0.0001/SF),
#      ACCEL_TPCH_DB (database file, default ../accelerator-codegen-duckdb/tpch_sf${SF}.duckdb),
#      QUERIES (comma list, default all 22), ACCEL_DUCKDB_ROOT (sibling DuckDB checkout),
#      BUILD_DIR (default build-stateless), OUT_DIR (default eval/bench/tpch/results).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THREADS="${1:-16,32}"
SEEDS="${2:-20}"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build-stateless}"
OUT_DIR="${OUT_DIR:-${ROOT}/eval/bench/tpch/results}"
SF="${SF:-1}"
QUERIES="${QUERIES:-$(seq -s, 1 22)}"
export ACCEL_DUCKDB_ROOT="${ACCEL_DUCKDB_ROOT:-$(cd "${ROOT}/../accelerator-codegen-duckdb" && pwd)}"
export ACCEL_TPCH_DB="${ACCEL_TPCH_DB:-${ACCEL_DUCKDB_ROOT}/tpch_sf${SF}.duckdb}"
[[ -f "$ACCEL_TPCH_DB" ]] || { echo "no database at $ACCEL_TPCH_DB" >&2; exit 1; }
COMMON=(--queries "$QUERIES" --scale-factor "$SF" --seeds "$SEEDS")
mkdir -p "$OUT_DIR"
STAMP="sf${SF}_$(uname -n | cut -d. -f1)_$(date +%Y%m%d_%H%M%S)"

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DACCEL_CODEGEN_ENGINE=duckdb \
      -DACCEL_ENABLE_PARALLEL_TPCH=ON >/dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)" --target stateless_matrix >/dev/null

echo "=== correctness (db=${ACCEL_TPCH_DB}, seeds=${SEEDS}) ==="
for t in 1 ${THREADS//,/ }; do
  "$BUILD_DIR/stateless_matrix" --mode check "${COMMON[@]}" --threads "$t" \
      > "$OUT_DIR/stateless_duckdb_check_t${t}_${STAMP}.csv" || true
  echo "threads=${t}: $(tail -n 1 "$OUT_DIR/stateless_duckdb_check_t${t}_${STAMP}.csv")"
done

echo "=== sweep (threads=${THREADS}, seeds=${SEEDS}) ==="
OUT="$OUT_DIR/stateless_duckdb_sweep_${STAMP}.csv"
"$BUILD_DIR/stateless_matrix" --mode sweep "${COMMON[@]}" --threads-list "$THREADS" > "$OUT"
grep '^#' "$OUT"
echo "raw: $OUT"
