#!/usr/bin/env bash
# Run the full concurrent-accelerator experiment suite:
#  1) Per-query multi-connection workload sweep (primary fair perf story)
#  2) Cross-accelerator writer-scheduled warehouse sweep
#  3) Cross-accelerator interference matrix (isolation)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-release"
RESULTS="${BUILD}/concurrent_experiments"

mkdir -p "$RESULTS"

echo "========== [1/3] Per-query multi-connection workload sweep =========="
ACCEL_WORKLOAD_DURATION_SEC="${ACCEL_WORKLOAD_DURATION_SEC:-25}" \
ACCEL_WORKLOAD_CALIBRATION_SEC="${ACCEL_WORKLOAD_CALIBRATION_SEC:-12}" \
ACCEL_WORKLOAD_READERS="${ACCEL_WORKLOAD_READERS:-4}" \
OUT="${RESULTS}/aux_workload_multi_connection_sweep.csv" \
bash "${ROOT}/scripts/run_aux_workload_multi_connection_sweep.sh" \
  2>&1 | tee "${RESULTS}/aux_workload_multi_connection_sweep.log"

cp -f "${RESULTS}/aux_workload_multi_connection_sweep.csv" "${BUILD}/aux_workload_multi_connection_sweep.csv" 2>/dev/null || true
cp -f "${BUILD}/aux_workload_multi_connection_sweep.summary.md" "${RESULTS}/" 2>/dev/null || true

echo "========== [2/3] Cross-accelerator writer-scheduled sweep =========="
ACCEL_CROSS_ACCEL_DURATION_SEC="${ACCEL_CROSS_ACCEL_DURATION_SEC:-25}" \
ACCEL_CROSS_ACCEL_CALIBRATION_SEC="${ACCEL_CROSS_ACCEL_CALIBRATION_SEC:-12}" \
ACCEL_CROSS_ACCEL_READERS="${ACCEL_CROSS_ACCEL_READERS:-6}" \
OUT="${RESULTS}/cross_accel_writer_scheduled_sweep.csv" \
LOG_DIR="${RESULTS}/cross_accel_writer_scheduled_logs" \
bash "${ROOT}/scripts/cross_accel_writer_scheduled_sweep.sh" \
  2>&1 | tee "${RESULTS}/cross_accel_writer_scheduled_sweep.log"

cp -f "${RESULTS}/cross_accel_writer_scheduled_sweep.summary.md" "${RESULTS}/" 2>/dev/null || true

echo "========== [3/3] Cross-accelerator interference matrix =========="
ACCEL_CROSS_ACCEL_DURATION_SEC="${ACCEL_CROSS_ACCEL_DURATION_SEC:-20}" \
ACCEL_CROSS_ACCEL_READERS="${ACCEL_CROSS_ACCEL_READERS:-5}" \
OUT="${RESULTS}/cross_accel_interference_matrix.csv" \
LOG_DIR="${RESULTS}/cross_accel_interference_logs" \
PNG="${RESULTS}/cross_accel_interference_matrix.png" \
bash "${ROOT}/scripts/cross_accel_interference_matrix.sh" \
  2>&1 | tee "${RESULTS}/cross_accel_interference_matrix.log" || true

python3 "${ROOT}/scripts/write_concurrent_experiments_report.py" "${RESULTS}" 2>/dev/null || true

echo "Done. Results in ${RESULTS}/"
