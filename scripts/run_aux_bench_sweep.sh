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
  DB_LABEL="${ACCEL_AUX_BENCH_DB_LABEL:-$(basename "${SOURCE_DB}" .duckdb)}"
fi

WORK_DIR="${ACCEL_AUX_BENCH_WORK_DIR:-${BUILD_DIR}/q${QUERY}_aux_bench_${DB_LABEL}}"
mkdir -p "${WORK_DIR}"
rm -f "${WORK_DIR}"/*.duckdb 2>/dev/null || true
RESULTS="${WORK_DIR}/results.csv"

if [[ ! -x "${BENCH_BIN}" ]]; then
  echo "missing benchmark binary: ${BENCH_BIN}" >&2
  echo "build with: cmake --build ${BUILD_DIR} --target ${BENCH_TARGET}" >&2
  exit 1
fi

export ACCEL_BENCH_WARMUP="${ACCEL_BENCH_WARMUP:-10}"
export ACCEL_BENCH_QUERIES="${ACCEL_BENCH_QUERIES:-100}"
export ACCEL_BENCH_REF_SAMPLES="${ACCEL_BENCH_REF_SAMPLES:-3}"
export ACCEL_BENCH_BASE_WRITE_SAMPLES="${ACCEL_BENCH_BASE_WRITE_SAMPLES:-3}"
export ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB="${ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB:-1}"
export ACCEL_REPO_ROOT="${ACCEL_REPO_ROOT:-${ROOT}}"
export ACCEL_AUX_BENCH_BASELINE_CACHE="${ACCEL_AUX_BENCH_BASELINE_CACHE:-${ROOT}/accelerators/${ACCEL_AUX_BENCHMARK:-tpch}/q${QUERY}/config/q${QUERY}_aux_benchmark_baseline.json}"
export ACCEL_AUX_BENCH_DB_LABEL="${ACCEL_AUX_BENCH_DB_LABEL:-${DB_LABEL}}"
# Fair by default: full SF1 scans, measured DuckDB reference. Set ACCEL_BENCH_FAST=1 for gate smoke.
if [[ "${ACCEL_BENCH_FAST:-0}" == "1" ]]; then
  export ACCEL_BENCH_SKIP_DUCKDB_REF="${ACCEL_BENCH_SKIP_DUCKDB_REF:-1}"
  export ACCEL_Q21_BENCH_SCAN_LIMIT="${ACCEL_Q21_BENCH_SCAN_LIMIT:-100000}"
  export ACCEL_BENCH_WARMUP="${ACCEL_BENCH_WARMUP:-1}"
  export ACCEL_BENCH_QUERIES="${ACCEL_BENCH_QUERIES:-5}"
  export ACCEL_BENCH_REF_SAMPLES="${ACCEL_BENCH_REF_SAMPLES:-0}"
else
  unset ACCEL_BENCH_SKIP_DUCKDB_REF
  unset ACCEL_Q21_BENCH_SCAN_LIMIT
fi

# Interval sweeps already vary write cadence in-process. Do not also loop
# write-frequency % (that re-ran the full interval grid 4x and wasted hours).
if [[ -n "${ACCEL_AUX_WRITE_INTERVALS:-}" ]]; then
  FREQUENCIES="${ACCEL_AUX_WRITE_FREQUENCIES:-0}"
else
  FREQUENCIES="${ACCEL_AUX_WRITE_FREQUENCIES:-0,1,10,50}"
fi
INTERVALS="${ACCEL_AUX_WRITE_INTERVALS:-}"
SUITES_VAR="ACCEL_Q${QUERY}_BENCH_SUITES"
SUITES="${!SUITES_VAR:-${ACCEL_AUX_BENCH_SUITES:-lineitem_only}}"

export ACCEL_AUX_BENCH_DURATION_SEC="${ACCEL_AUX_BENCH_DURATION_SEC:-120}"
export ACCEL_AUX_BENCH_MIN_WRITES="${ACCEL_AUX_BENCH_MIN_WRITES:-30}"

IFS=',' read -r -a freq_list <<< "${FREQUENCIES}"
IFS=',' read -r -a suite_list <<< "${SUITES}"
interval_list=()
if [[ -n "${INTERVALS}" ]]; then
  IFS=',' read -r -a interval_list <<< "${INTERVALS}"
fi

filter_csv_output() {
  # fflush after every line so tee updates results.csv immediately (stdout is a pipe).
  awk -F, '
    /^suite,/ { if (!header) { print; fflush(); header = 1 }; next }
    /^speedup_gate/ { next }
    /^#/ { next }
    NF >= 10 { print; fflush() }
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

  bench_db="${WORK_DIR}/${DB_LABEL}_${suite}.duckdb"
  rm -f "${bench_db}"
  cp -l "${SOURCE_DB}" "${bench_db}" 2>/dev/null || cp "${SOURCE_DB}" "${bench_db}"

  schedule_csv=""
  if [[ "${#interval_list[@]}" -gt 0 ]]; then
    schedule_csv="$(IFS=','; echo "${interval_list[*]}")"
    echo "# query=${QUERY} benchmark=${BENCHMARK} suite=${suite} write_intervals_sec=${schedule_csv}" >&2
  else
    schedule_csv="$(IFS=','; echo "${active_freq_list[*]}")"
    echo "# query=${QUERY} benchmark=${BENCHMARK} suite=${suite} write_frequencies=${schedule_csv}" >&2
  fi

  run_bench() {
    local env_prefix=()
    env_prefix+=("${DB_ENV_VAR}=${bench_db}")
    env_prefix+=("ACCEL_Q${QUERY}_BENCH_SUITE=${suite}")
    if [[ "${#interval_list[@]}" -gt 0 ]]; then
      env_prefix+=("ACCEL_AUX_WRITE_INTERVALS=${schedule_csv}")
      env_prefix+=("ACCEL_AUX_BENCH_DURATION_SEC=${ACCEL_AUX_BENCH_DURATION_SEC}")
      env_prefix+=("ACCEL_AUX_BENCH_MIN_WRITES=${ACCEL_AUX_BENCH_MIN_WRITES}")
    else
      env_prefix+=("ACCEL_AUX_WRITE_FREQUENCIES=${schedule_csv}")
    fi
    for var in BENCH_MONTH BENCH_DAY_OFFSET BENCH_SEGMENT BENCH_CUTOFF_DATE WRITE_LOCALITY BENCH_TRACE; do
      local full="ACCEL_Q${QUERY}_${var}"
      if [[ -n "${!full:-}" ]]; then
        env_prefix+=("${full}=${!full}")
      fi
    done
    local unset_args=(-u "ACCEL_Q${QUERY}_WRITE_FREQUENCY_PCT")
    if [[ "${#interval_list[@]}" -gt 0 ]]; then
      unset_args+=(-u ACCEL_AUX_WRITE_FREQUENCIES)
    else
      unset_args+=(-u ACCEL_AUX_WRITE_INTERVALS)
    fi
    env "${unset_args[@]}" "${env_prefix[@]}" "${BENCH_BIN}"
  }

  if [[ "${printed_header}" == false ]]; then
    run_bench | filter_csv_output | tee "${RESULTS}"
    printed_header=true
  else
    run_bench | filter_csv_output | tail -n +2 >> "${RESULTS}"
  fi
  rm -f "${bench_db}"
done

echo "wrote ${RESULTS}" >&2
echo "${RESULTS}"
