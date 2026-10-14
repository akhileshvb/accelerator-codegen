#!/usr/bin/env bash
# Run full parallel TPC-H thread-scaling matrix (SF1 and/or SF10).
#
# Usage:
#   ./scripts/run_parallel_scaling_matrix.sh
#   ./scripts/run_parallel_scaling_matrix.sh --scale-factor 1 --threads 1,2,4,8,16 --seeds 1-5
#   ./scripts/run_parallel_scaling_matrix.sh --scale-factor 10 --threads 1,2,4,8 --seeds 1-5
set -euo pipefail

ROOT=/home/akhilvb/accelerator-codegen
SCRIPT="${ROOT}/scripts/run_validated_tpch_bench.sh"
OUT_DIR="${ROOT}/eval/bench/tpch/results/parallel_matrix"
SCALE_LIST="1"
THREAD_LIST="1,2,4,8,16"
SEEDS="1-5"
SESSION=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scale-factor) SCALE_LIST="${2:?}"; shift 2 ;;
    --threads) THREAD_LIST="${2:?}"; shift 2 ;;
    --seeds) SEEDS="${2:?}"; shift 2 ;;
    --session) SESSION="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '2,10p' "$0"
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

mkdir -p "${OUT_DIR}"
HOST_META="${OUT_DIR}/host_topology.txt"
{
  echo "date=$(date -Is)"
  lscpu | grep -E 'Model name|Socket|Core|Thread|NUMA|CPU\(s\)' || true
  echo "nproc=$(nproc)"
  free -h | head -2
  echo "uname=$(uname -a)"
} >"${HOST_META}"

SUMMARY="${OUT_DIR}/matrix_summary_session${SESSION}.csv"
echo "scale,threads,mode,csv_path,status" >"${SUMMARY}"

IFS=',' read -r -a SCALES <<< "${SCALE_LIST}"
IFS=',' read -r -a THREADS <<< "${THREAD_LIST}"

for scale in "${SCALES[@]}"; do
  for threads in "${THREADS[@]}"; do
    echo "=== matrix SF${scale} parallel threads=${threads} ==="
    if "${SCRIPT}" --scale-factor "${scale}" --session "${SESSION}" --seeds "${SEEDS}" \
        --accelerator-mode parallel --threads "${threads}"; then
      csv="${ROOT}/eval/bench/tpch/results/validated_sf${scale}_session${SESSION}_parallel_t${threads}.csv"
      # Copy into matrix folder for archival
      if [[ -f "${csv}" ]]; then
        cp -f "${csv}" "${OUT_DIR}/"
        echo "${scale},${threads},parallel,${csv},ok" >>"${SUMMARY}"
      else
        echo "${scale},${threads},parallel,missing,ok_no_csv" >>"${SUMMARY}"
      fi
    else
      echo "${scale},${threads},parallel,,FAILED" >>"${SUMMARY}"
      echo "FAILED SF${scale} t=${threads}" >&2
    fi
  done
done

# Also capture single-threaded accel at threads=1 for self-scaling baseline (SF1 only by default)
for scale in "${SCALES[@]}"; do
  echo "=== matrix SF${scale} single threads=1 (self-scaling baseline) ==="
  if "${SCRIPT}" --scale-factor "${scale}" --session "${SESSION}" --seeds "${SEEDS}" \
      --accelerator-mode single --threads 1; then
    csv="${ROOT}/eval/bench/tpch/results/validated_sf${scale}_session${SESSION}_single_t1.csv"
    if [[ -f "${csv}" ]]; then
      cp -f "${csv}" "${OUT_DIR}/"
      echo "${scale},1,single,${csv},ok" >>"${SUMMARY}"
    fi
  else
    echo "${scale},1,single,,FAILED" >>"${SUMMARY}"
  fi
done

echo "=== matrix complete; summary ${SUMMARY} ==="
cat "${SUMMARY}"
