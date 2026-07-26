#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
BENCH_BIN="${BUILD_DIR}/benchmark_q14_hybrid_eval"
SOURCE_DB="${ACCEL_TPCH_DB:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
WORK_DIR="${ACCEL_Q14_CROSSOVER_WORK_DIR:-${BUILD_DIR}/q14_crossover_sf1}"
mkdir -p "${WORK_DIR}"
mkdir -p "$(dirname "${WORK_DIR}/run.log")"

if [[ ! -x "${BENCH_BIN}" ]]; then
  echo "missing benchmark binary: ${BENCH_BIN}" >&2
  echo "build with: cmake --build ${BUILD_DIR} --target benchmark_q14_hybrid_eval" >&2
  exit 1
fi

export ACCEL_Q14_EVAL_MODE=crossover
export ACCEL_BENCH_QUERIES="${ACCEL_BENCH_QUERIES:-500}"
export ACCEL_BENCH_WARMUP="${ACCEL_BENCH_WARMUP:-50}"
export ACCEL_Q14_QUERIES_PER_WRITE="${ACCEL_Q14_QUERIES_PER_WRITE:-10}"

POLICIES=(hybrid)

declare -A AXIS_VALUES=(
  [lineitem_rows]="1,10,100,1000,10000,100000,full_month"
  [part_keys]="1,2,5,10,25,50"
  [fanout_bucket]="low,medium,high"
  [partitions]="1,2,4,all"
  [queries_per_write]="1,2,5,10,50,100"
)

AXES="${ACCEL_Q14_CROSSOVER_AXES:-lineitem_rows,part_keys,fanout_bucket,partitions,queries_per_write}"
RESULTS="${WORK_DIR}/crossover_results.csv"
: > "${RESULTS}"

IFS=',' read -r -a axis_list <<< "${AXES}"
for axis in "${axis_list[@]}"; do
  IFS=',' read -r -a value_list <<< "${AXIS_VALUES[$axis]}"
  for value in "${value_list[@]}"; do
    for policy in "${POLICIES[@]}"; do
      bench_db="${WORK_DIR}/tpch_${axis}_${value}_${policy}.duckdb"
      cp "${SOURCE_DB}" "${bench_db}"
      echo "# axis=${axis} value=${value} policy=${policy}" >&2
      if [[ ! -s "${RESULTS}" ]]; then
        ACCEL_TPCH_DB="${bench_db}" \
          ACCEL_Q14_CROSSOVER_AXIS="${axis}" \
          ACCEL_Q14_CROSSOVER_VALUE="${value}" \
          ACCEL_Q14_EVAL_PRINT_HEADER=1 \
          "${BENCH_BIN}" | tee -a "${RESULTS}"
      else
        ACCEL_TPCH_DB="${bench_db}" \
          ACCEL_Q14_CROSSOVER_AXIS="${axis}" \
          ACCEL_Q14_CROSSOVER_VALUE="${value}" \
          "${BENCH_BIN}" >> "${RESULTS}"
      fi
      rm -f "${bench_db}"
    done
  done
done

python3 "${ROOT}/scripts/plot_q14_crossover_bench.py" --csv "${RESULTS}" --out-dir "${WORK_DIR}/plots"
echo "wrote ${RESULTS}" >&2
