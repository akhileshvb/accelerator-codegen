#!/usr/bin/env bash
# Full audited parallel TPC-H validated matrix (2 sessions).
set -euo pipefail
ROOT=/home/akhilvb/accelerator-codegen
MATRIX="${ROOT}/scripts/run_parallel_scaling_matrix.sh"
OUT="${ROOT}/benchmarks/tpch/results/parallel_matrix_audit"
LOG="${OUT}/full_campaign.log"
mkdir -p "${OUT}"

{
  echo "=== campaign start $(date -Is) ==="
  echo "protocol: seeds=1-5 warmup=3 iters=5"
  echo "SF1 threads=1,2,4,8,16 ; SF10 threads=1,4,8,16 ; 2 sessions"
  echo "build=build-release-parallel -DACCEL_ENABLE_PARALLEL_TPCH=ON Release -O3 -DNDEBUG"

  for session in 1 2; do
    echo "=== SESSION ${session} SF1 ==="
    "${MATRIX}" --scale-factor 1 --threads 1,2,4,8,16 --seeds 1-5 --session "${session}"
    echo "=== SESSION ${session} SF10 ==="
    "${MATRIX}" --scale-factor 10 --threads 1,4,8,16 --seeds 1-5 --session "${session}"
  done

  echo "=== aggregating ==="
  python3 "${ROOT}/scripts/aggregate_parallel_matrix.py" \
    --out "${OUT}/full_matrix_summary.csv" \
    --st-baseline \
      "${ROOT}/benchmarks/tpch/results/validated_sf1_session1_single_t1.csv" \
      "${ROOT}/benchmarks/tpch/results/validated_sf1_session2_single_t1.csv" \
      "${ROOT}/benchmarks/tpch/results/validated_sf10_session1_single_t1.csv" \
      "${ROOT}/benchmarks/tpch/results/validated_sf10_session2_single_t1.csv" \
    "${ROOT}/benchmarks/tpch/results"/validated_sf*_session*_parallel_t*.csv \
    || true

  # Also copy key CSVs into OUT
  cp -f "${ROOT}/benchmarks/tpch/results"/validated_sf*_session*_parallel_t*.csv "${OUT}/" 2>/dev/null || true
  cp -f "${ROOT}/benchmarks/tpch/results"/validated_sf*_session*_single_t1.csv "${OUT}/" 2>/dev/null || true

  echo "=== campaign end $(date -Is) ==="
} 2>&1 | tee "${LOG}"
