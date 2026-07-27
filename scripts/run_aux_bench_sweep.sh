#!/usr/bin/env bash
# Sweep point-write frequency for an aux-state benchmark (0%–50% by default).
# Usage: run_aux_bench_sweep.sh <query_number>
# Env: ACCEL_BUILD_DIR, ACCEL_TPCH_DB, ACCEL_AUX_WRITE_FREQUENCIES, ACCEL_AUX_BENCH_WORK_DIR

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUERY="${1:-${ACCEL_AUX_QUERY:-}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_bench_sweep.sh <query_number>" >&2
  exit 1
fi

BUILD_DIR="${ACCEL_BUILD_DIR:-${ROOT}/build-release}"
BENCH_BIN="${BUILD_DIR}/benchmark_tpch_q${QUERY}_aux"
SOURCE_DB="${ACCEL_TPCH_DB:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
WORK_DIR="${ACCEL_AUX_BENCH_WORK_DIR:-${BUILD_DIR}/q${QUERY}_aux_bench_sf1}"
mkdir -p "${WORK_DIR}"
RESULTS="${WORK_DIR}/results.csv"

if [[ ! -x "${BENCH_BIN}" ]]; then
  echo "missing benchmark binary: ${BENCH_BIN}" >&2
  echo "build with: cmake --build ${BUILD_DIR} --target benchmark_tpch_q${QUERY}_aux" >&2
  exit 1
fi

export ACCEL_BENCH_WARMUP="${ACCEL_BENCH_WARMUP:-50}"
export ACCEL_BENCH_QUERIES="${ACCEL_BENCH_QUERIES:-500}"
export ACCEL_BENCH_REF_SAMPLES="${ACCEL_BENCH_REF_SAMPLES:-30}"
export ACCEL_BENCH_BASE_WRITE_SAMPLES="${ACCEL_BENCH_BASE_WRITE_SAMPLES:-10}"

FREQUENCIES="${ACCEL_AUX_WRITE_FREQUENCIES:-0,1,10,50}"
SUITES_VAR="ACCEL_Q${QUERY}_BENCH_SUITES"
SUITES="${!SUITES_VAR:-${ACCEL_AUX_BENCH_SUITES:-lineitem_only}}"

IFS=',' read -r -a freq_list <<< "${FREQUENCIES}"
IFS=',' read -r -a suite_list <<< "${SUITES}"

: > "${RESULTS}"
printed_header=false

for suite in "${suite_list[@]}"; do
  if [[ "${suite}" == bulk_* ]]; then
    BULK_VAR="ACCEL_Q${QUERY}_BULK_WRITE_FREQUENCIES"
    IFS=',' read -r -a active_freq_list <<< "${!BULK_VAR:-${ACCEL_AUX_BULK_WRITE_FREQUENCIES:-0,1,10}}"
  else
    active_freq_list=("${freq_list[@]}")
  fi
  for freq in "${active_freq_list[@]}"; do
    bench_db="${WORK_DIR}/tpch_sf1_${suite}_${freq}.duckdb"
    cp "${SOURCE_DB}" "${bench_db}"
    echo "# query=${QUERY} suite=${suite} write_frequency_pct=${freq}" >&2

    run_bench() {
      local env_prefix=()
      env_prefix+=("ACCEL_TPCH_DB=${bench_db}")
      env_prefix+=("ACCEL_Q${QUERY}_BENCH_SUITE=${suite}")
      env_prefix+=("ACCEL_Q${QUERY}_WRITE_FREQUENCY_PCT=${freq}")
      # Pass through query-specific bench env vars when set.
      for var in BENCH_MONTH BENCH_DAY_OFFSET WRITE_LOCALITY BENCH_TRACE; do
        local full="ACCEL_Q${QUERY}_${var}"
        if [[ -n "${!full:-}" ]]; then
          env_prefix+=("${full}=${!full}")
        fi
      done
      env "${env_prefix[@]}" "${BENCH_BIN}"
    }

    if [[ "${printed_header}" == false ]]; then
      run_bench | tee "${RESULTS}"
      printed_header=true
    else
      run_bench | tail -n +2 >> "${RESULTS}"
    fi
    rm -f "${bench_db}"
  done
done

echo "wrote ${RESULTS}" >&2
echo "${RESULTS}"
