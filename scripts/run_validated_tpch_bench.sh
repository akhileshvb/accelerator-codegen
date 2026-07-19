#!/usr/bin/env bash
# Clean, reproducible validated TPC-H benchmark for all implemented accelerators:
# Q1, Q3, Q4, Q6, Q9, Q12, Q14.
#
# Production kernel paths are hardcoded (no ambient ACCEL_* required).
# This script clears experimental ACCEL_* / DUCKDB_* vars so shell inheritance cannot skew results.
set -euo pipefail

ROOT=/home/akhilvb/accelerator-codegen
DUCKDB_ROOT=/home/akhilvb/accelerator-codegen-duckdb
BUILD="${ROOT}/build-release"
OUT_DIR="${ROOT}/benchmarks/tpch/results"
SESSION="${1:-1}"

mkdir -p "${OUT_DIR}"

echo "=== build identity ==="
echo "accelerator_commit=$(git -C "${ROOT}" rev-parse HEAD)"
echo "duckdb_commit=$(git -C "${DUCKDB_ROOT}" rev-parse HEAD)"
echo "compiler=$(c++ --version | head -1)"
echo "duckdb_build_type=$(rg 'CMAKE_BUILD_TYPE:' "${DUCKDB_ROOT}/build/release/CMakeCache.txt" | head -1)"
echo "duckdb_cxx_flags_release=$(rg 'CMAKE_CXX_FLAGS_RELEASE:' "${DUCKDB_ROOT}/build/release/CMakeCache.txt" | head -1)"
echo "accel_build_type=$(rg 'CMAKE_BUILD_TYPE:' "${BUILD}/CMakeCache.txt" | head -1)"
echo "accel_cxx_flags_release=$(rg 'CMAKE_CXX_FLAGS_RELEASE:' "${BUILD}/CMakeCache.txt" | head -1)"
echo "libduckdb=$(readlink -f "${DUCKDB_ROOT}/build/release/src/libduckdb.so")"
echo "libduckdb_mtime=$(stat -c '%y' "${DUCKDB_ROOT}/build/release/src/libduckdb.so")"

# Drop any inherited experimental toggles that could affect DuckDB-side paths.
while IFS= read -r name; do
  unset "${name}" || true
done < <(env | awk -F= '/^ACCEL_Q|^DUCKDB_BITPACKING|^DUCKDB_Q6_/ {print $1}')

export ACCEL_TPCH_DB="${DUCKDB_ROOT}/tpch_sf1.duckdb"
export ACCEL_BENCH_WARMUP=3
export ACCEL_BENCH_ITERS=10
export ACCEL_BENCH_SESSION="${SESSION}"
export DUCKDB_BITPACKING_NATIVE_FILTER=0
export ACCEL_Q1_AUX_STATE=0

cmake --build "${BUILD}" -j "$(nproc)" --target benchmark_tpch_validated

CSV="${OUT_DIR}/validated_session${SESSION}.csv"
LOG="${OUT_DIR}/validated_session${SESSION}.log"
echo "=== running session ${SESSION} -> ${CSV} ==="
env -u ACCEL_Q1_FUSED_SCAN -u ACCEL_Q1_FUSED_ALIGNED_PAYLOAD -u ACCEL_Q3_DENSE_CUST \
  -u ACCEL_Q4_LINEITEM_FUSED -u ACCEL_Q9_DENSE_ORDERS -u ACCEL_Q12_DENSE_ORDERS -u ACCEL_Q14_DENSE_PART \
  "${BUILD}/benchmark_tpch_validated" >"${CSV}" 2>"${LOG}"
echo "=== session ${SESSION} complete ==="
tail -n 40 "${LOG}"
