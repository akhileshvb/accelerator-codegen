#!/usr/bin/env bash
# Run an aux-state benchmark sweep.
# - With ACCEL_AUX_WRITE_INTERVALS: one pass (interval sweep lives in the binary).
# - Otherwise: sweep write frequency (0%–50% by default).
# Usage: run_aux_bench_sweep.sh <query_number>
# Env: ACCEL_BUILD_DIR, ACCEL_AUX_BENCH_TARGET, ACCEL_AUX_BENCHMARK, ACCEL_SEC_EDGAR_DB / ACCEL_TPCH_DB,
#      ACCEL_AUX_WRITE_INTERVALS, ACCEL_AUX_WRITE_FREQUENCIES, ACCEL_AUX_BENCH_WORK_DIR,
#      ACCEL_AUX_BENCH_SUITES

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUERY="${1:-${ACCEL_AUX_QUERY:-}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_bench_sweep.sh <query_number>" >&2
  exit 1
fi

BENCHMARK="${ACCEL_AUX_BENCHMARK:-tpch}"
BUILD_DIR="${ACCEL_BUILD_DIR:-${ROOT}/build-release}"
BENCH_TARGET="${ACCEL_AUX_BENCH_TARGET:-benchmark_${BENCHMARK}_q${QUERY}_aux}"
BENCH_BIN="${BUILD_DIR}/${BENCH_TARGET}"

if [[ "${BENCHMARK}" == "sec_edgar" ]]; then
  SOURCE_DB="${ACCEL_SEC_EDGAR_DB:-/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
  DB_ENV_VAR="ACCEL_SEC_EDGAR_DB"
  DB_LABEL="sec_edgar_sf3"
else
  SOURCE_DB="${ACCEL_TPCH_DB:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
  DB_ENV_VAR="ACCEL_TPCH_DB"
  DB_LABEL="tpch_sf1"
fi

WORK_DIR="${ACCEL_AUX_BENCH_WORK_DIR:-${BUILD_DIR}/q${QUERY}_aux_bench_${DB_LABEL}}"
mkdir -p "${WORK_DIR}"
RESULTS="${WORK_DIR}/results.csv"

if [[ ! -x "${BENCH_BIN}" ]]; then
  echo "missing benchmark binary: ${BENCH_BIN}" >&2
  echo "build with: cmake --build ${BUILD_DIR} --target ${BENCH_TARGET}" >&2
  exit 1
fi

export ACCEL_BENCH_WARMUP="${ACCEL_BENCH_WARMUP:-10}"
export ACCEL_BENCH_QUERIES="${ACCEL_BENCH_QUERIES:-100}"
export ACCEL_BENCH_REF_SAMPLES="${ACCEL_BENCH_REF_SAMPLES:-5}"
export ACCEL_BENCH_BASE_WRITE_SAMPLES="${ACCEL_BENCH_BASE_WRITE_SAMPLES:-3}"
export ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB="${ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB:-1}"

# Interval sweeps already vary write cadence in-process. Do not also loop
# write-frequency % (that re-ran the full interval grid 4x and wasted hours).
if [[ -n "${ACCEL_AUX_WRITE_INTERVALS:-}" ]]; then
  FREQUENCIES="${ACCEL_AUX_WRITE_FREQUENCIES:-0}"
else
  FREQUENCIES="${ACCEL_AUX_WRITE_FREQUENCIES:-0,1,10,50}"
fi
SUITES_VAR="ACCEL_Q${QUERY}_BENCH_SUITES"
SUITES="${!SUITES_VAR:-${ACCEL_AUX_BENCH_SUITES:-lineitem_only}}"

IFS=',' read -r -a freq_list <<< "${FREQUENCIES}"
IFS=',' read -r -a suite_list <<< "${SUITES}"

filter_csv_output() {
  awk -F, '
    /^suite,/ { if (!header) { print; header = 1 }; next }
    /^speedup_gate/ { next }
    /^#/ { next }
    NF >= 10 { print }
  '
}

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
    bench_db="${WORK_DIR}/${DB_LABEL}_${suite}_${freq}.duckdb"
    cp "${SOURCE_DB}" "${bench_db}"
    echo "# query=${QUERY} benchmark=${BENCHMARK} suite=${suite} write_frequency_pct=${freq}" >&2

    run_bench() {
      local env_prefix=()
      env_prefix+=("${DB_ENV_VAR}=${bench_db}")
      env_prefix+=("ACCEL_Q${QUERY}_BENCH_SUITE=${suite}")
      env_prefix+=("ACCEL_Q${QUERY}_WRITE_FREQUENCY_PCT=${freq}")
      for var in BENCH_MONTH BENCH_DAY_OFFSET BENCH_SEGMENT BENCH_CUTOFF_DATE WRITE_LOCALITY BENCH_TRACE; do
        local full="ACCEL_Q${QUERY}_${var}"
        if [[ -n "${!full:-}" ]]; then
          env_prefix+=("${full}=${!full}")
        fi
      done
      env "${env_prefix[@]}" "${BENCH_BIN}"
    }

    if [[ "${printed_header}" == false ]]; then
      run_bench | filter_csv_output | tee "${RESULTS}"
      printed_header=true
    else
      run_bench | filter_csv_output | tail -n +2 >> "${RESULTS}"
    fi
    rm -f "${bench_db}"
  done
done

echo "wrote ${RESULTS}" >&2
echo "${RESULTS}"
