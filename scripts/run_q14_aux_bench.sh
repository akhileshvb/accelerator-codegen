#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ACCEL_BUILD_DIR:-${ROOT}/build-release}"
BENCH_BIN="${BUILD_DIR}/benchmark_tpch_q14_aux"
SOURCE_DB="${ACCEL_TPCH_DB:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
WORK_DIR="${ACCEL_Q14_BENCH_WORK_DIR:-${BUILD_DIR}/q14_aux_bench_work_$$}"
mkdir -p "${WORK_DIR}"

if [[ ! -x "${BENCH_BIN}" ]]; then
  echo "missing benchmark binary: ${BENCH_BIN}" >&2
  echo "build with: cmake --build ${BUILD_DIR} --target benchmark_tpch_q14_aux" >&2
  exit 1
fi

export ACCEL_BENCH_WARMUP="${ACCEL_BENCH_WARMUP:-100}"
export ACCEL_BENCH_QUERIES="${ACCEL_BENCH_QUERIES:-1000}"
export ACCEL_BENCH_REF_SAMPLES="${ACCEL_BENCH_REF_SAMPLES:-50}"
export ACCEL_BENCH_BASE_WRITE_SAMPLES="${ACCEL_BENCH_BASE_WRITE_SAMPLES:-20}"
export ACCEL_Q14_WRITE_LOCALITY="${ACCEL_Q14_WRITE_LOCALITY:-single_month}"
export ACCEL_Q14_BENCH_MONTH="${ACCEL_Q14_BENCH_MONTH:-1995-06-01}"

FREQUENCIES="${ACCEL_Q14_WRITE_FREQUENCIES:-0,1,10,50}"
SUITES="${ACCEL_Q14_BENCH_SUITES:-lineitem_only,part_only,bulk_lineitem_month,bulk_part_month}"
POLICIES="${Q14_BENCH_WRITE_POLICIES:-hybrid,force_invalidate,force_incremental,force_rebuild,oracle,table,relevant,incremental,rebuild}"

IFS=',' read -r -a freq_list <<< "${FREQUENCIES}"
IFS=',' read -r -a suite_list <<< "${SUITES}"
IFS=',' read -r -a policy_list <<< "${POLICIES}"

printed_header=false

for suite in "${suite_list[@]}"; do
  if [[ "${suite}" == bulk_* ]]; then
    IFS=',' read -r -a active_freq_list <<< "${ACCEL_Q14_BULK_WRITE_FREQUENCIES:-0,1,10}"
  else
    active_freq_list=("${freq_list[@]}")
  fi
  for policy in "${policy_list[@]}"; do
    for freq in "${active_freq_list[@]}"; do
      bench_db="${WORK_DIR}/tpch_sf1_${suite}_${policy}_${freq}.duckdb"
      cp "${SOURCE_DB}" "${bench_db}"
      echo "# suite=${suite} write_policy=${policy} write_frequency_pct=${freq}" >&2
      if [[ "${printed_header}" == false ]]; then
        ACCEL_TPCH_DB="${bench_db}" \
          ACCEL_Q14_BENCH_SUITE="${suite}" \
          Q14_WRITE_POLICY="${policy}" \
          ACCEL_Q14_WRITE_FREQUENCY_PCT="${freq}" \
          "${BENCH_BIN}" | tee "${WORK_DIR}/results.csv"
        printed_header=true
      else
      ACCEL_TPCH_DB="${bench_db}" \
        ACCEL_Q14_BENCH_SUITE="${suite}" \
        Q14_WRITE_POLICY="${policy}" \
        ACCEL_Q14_WRITE_FREQUENCY_PCT="${freq}" \
        "${BENCH_BIN}" | tail -n +2 >> "${WORK_DIR}/results.csv"
      fi
      rm -f "${bench_db}"
    done
  done
done

echo "wrote ${WORK_DIR}/results.csv" >&2
