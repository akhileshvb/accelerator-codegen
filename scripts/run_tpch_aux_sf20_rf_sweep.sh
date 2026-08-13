#!/usr/bin/env bash
# TPC-H SF20 aux write-interval sweep using mini-chunk RF1/RF2 (orders+lineitem
# insert/delete), alternating RF2 then RF1 each write slot.
#
# Point lineitem UPDATE sweeps remain the default via run_tpch_aux_sf20_sweep.sh
# (ACCEL_AUX_WRITE_MODE=point). Use this script for the RF experiment.
#
# Mini-chunks (default 1 order) keep incremental maintenance measurable on SF20.
# Full official RF sets are ~0.1% of SF and would dominate with rebuilds.
#
# Usage:
#   bash scripts/run_tpch_aux_sf20_rf_sweep.sh
#   QUERIES=1,3,6,14,17,18 bash scripts/run_tpch_aux_sf20_rf_sweep.sh smoke
#   ACCEL_TPCH_RF_CHUNK_ORDERS=2 QUERIES=18 bash scripts/run_tpch_aux_sf20_rf_sweep.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ACCEL_AUX_WRITE_MODE="${ACCEL_AUX_WRITE_MODE:-rf}"
export ACCEL_TPCH_RF_CHUNK_ORDERS="${ACCEL_TPCH_RF_CHUNK_ORDERS:-1}"
# Exclude Q11 by default — RF does not touch partsupp/supplier/nation.
export QUERIES="${QUERIES:-1,3,6,9,10,12,14,17,18,21}"
export OUT_DIR="${OUT_DIR:-${ROOT}/benchmarks/tpch/results_rf}"
export LOG="${LOG:-${OUT_DIR}/tpch_aux_sf20_rf_sweep.log}"

echo "=== TPC-H SF20 RF mini-chunk aux sweep ==="
echo "ACCEL_AUX_WRITE_MODE=${ACCEL_AUX_WRITE_MODE}"
echo "ACCEL_TPCH_RF_CHUNK_ORDERS=${ACCEL_TPCH_RF_CHUNK_ORDERS}"
echo "QUERIES=${QUERIES}"
echo "Note: point UPDATE sweeps still work via run_tpch_aux_sf20_sweep.sh (default write_mode=point)."

exec bash "${ROOT}/scripts/run_tpch_aux_sf20_sweep.sh" "$@"
