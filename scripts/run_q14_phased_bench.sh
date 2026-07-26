#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
BENCH_BIN="${BUILD_DIR}/benchmark_q14_hybrid_eval"
SOURCE_DB="${ACCEL_TPCH_DB:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
WORK_DIR="${ACCEL_Q14_PHASED_WORK_DIR:-${BUILD_DIR}/q14_phased_sf1}"
mkdir -p "${WORK_DIR}"

if [[ ! -x "${BENCH_BIN}" ]]; then
  echo "missing benchmark binary: ${BENCH_BIN}" >&2
  exit 1
fi

bench_db="${WORK_DIR}/tpch_phased.duckdb"
timeline="${WORK_DIR}/phased_timeline.csv"
cp "${SOURCE_DB}" "${bench_db}"

ACCEL_TPCH_DB="${bench_db}" \
  ACCEL_Q14_EVAL_MODE=phased \
  ACCEL_Q14_PHASED_TIMELINE_CSV="${timeline}" \
  "${BENCH_BIN}" > "${WORK_DIR}/phased_summary.txt"

python3 "${ROOT}/scripts/plot_q14_phased_bench.py" --csv "${timeline}" --out-dir "${WORK_DIR}/plots"
echo "wrote ${timeline}" >&2
rm -f "${bench_db}"
