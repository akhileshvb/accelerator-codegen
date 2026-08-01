#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-release"
RESULTS="${BUILD}/concurrent_experiments"
mkdir -p "$RESULTS"

echo "========== [2/3] Cross-accelerator writer-scheduled sweep =========="
ACCEL_CROSS_ACCEL_DURATION_SEC="${ACCEL_CROSS_ACCEL_DURATION_SEC:-25}" \
ACCEL_CROSS_ACCEL_CALIBRATION_SEC="${ACCEL_CROSS_ACCEL_CALIBRATION_SEC:-12}" \
ACCEL_CROSS_ACCEL_READERS="${ACCEL_CROSS_ACCEL_READERS:-6}" \
OUT="${RESULTS}/cross_accel_writer_scheduled_sweep.csv" \
LOG_DIR="${RESULTS}/cross_accel_writer_scheduled_logs" \
bash "${ROOT}/scripts/cross_accel_writer_scheduled_sweep.sh" \
  2>&1 | tee "${RESULTS}/cross_accel_writer_scheduled_sweep.log"

cp -f "${RESULTS}/cross_accel_writer_scheduled_sweep.summary.md" "${BUILD}/" 2>/dev/null || true

echo "========== [3/3] Cross-accelerator interference matrix =========="
ACCEL_CROSS_ACCEL_DURATION_SEC="${ACCEL_CROSS_ACCEL_DURATION_SEC:-20}" \
ACCEL_CROSS_ACCEL_READERS="${ACCEL_CROSS_ACCEL_READERS:-5}" \
OUT="${RESULTS}/cross_accel_interference_matrix.csv" \
LOG_DIR="${RESULTS}/cross_accel_interference_logs" \
PNG="${RESULTS}/cross_accel_interference_matrix.png" \
bash "${ROOT}/scripts/cross_accel_interference_matrix.sh" \
  2>&1 | tee "${RESULTS}/cross_accel_interference_matrix.log" || true

python3 "${ROOT}/scripts/write_concurrent_experiments_report.py" "${RESULTS}" || true
echo "Done. Results in ${RESULTS}/"
