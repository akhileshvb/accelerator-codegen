#!/usr/bin/env bash
# Run SF20 Q20 phased baselines: always ApplyDelta and always RebuildAll.
#
# Prerequisites:
#   cmake --build build --target benchmark_tpch_q20_phased_eval
#   TPC-H SF20 DuckDB at ACCEL_TPCH_DB (default below)
#
# Runtime note:
#   always_rebuild rebuilds aux on every write (~87 writes). On SF20 each
#   RebuildAll is often ~2 minutes, so expect on the order of several hours.
#   always_incremental is typically much faster.
#
# After both finish, replot with measured overlays:
#   python3 scripts/plot_q20_phase_bands_styled.py \
#     --csv build/q20_phased_sf20/phased_timeline.csv \
#     --always-incremental-csv build/q20_phased_sf20/phased_timeline_always_incremental.csv \
#     --always-rebuild-csv build/q20_phased_sf20/phased_timeline_always_rebuild.csv \
#     --out-dir build/q20_phased_sf20/plots
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ACCEL_TPCH_DB="${ACCEL_TPCH_DB:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf20.duckdb}"
export ACCEL_Q20_PHASED_WORK_DIR="${ACCEL_Q20_PHASED_WORK_DIR:-${ROOT}/build/q20_phased_sf20}"
export ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL="${ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL:-${ROOT}/config/q20_aux_cost_model_phased.json}"

RUN_INC="${RUN_ALWAYS_INCREMENTAL:-1}"
RUN_REB="${RUN_ALWAYS_REBUILD:-1}"

echo "SF20 phased baselines" >&2
echo "  ACCEL_TPCH_DB=${ACCEL_TPCH_DB}" >&2
echo "  ACCEL_Q20_PHASED_WORK_DIR=${ACCEL_Q20_PHASED_WORK_DIR}" >&2

if [[ "${RUN_INC}" == "1" ]]; then
  echo "=== always_incremental (force ApplyDelta) ===" >&2
  ACCEL_Q20_PHASED_POLICY=always_incremental "${ROOT}/scripts/run_q20_phased_bench.sh"
fi

if [[ "${RUN_REB}" == "1" ]]; then
  echo "=== always_rebuild (force RebuildAll) ===" >&2
  ACCEL_Q20_PHASED_POLICY=always_rebuild "${ROOT}/scripts/run_q20_phased_bench.sh"
fi

INC_CSV="${ACCEL_Q20_PHASED_WORK_DIR}/phased_timeline_always_incremental.csv"
REB_CSV="${ACCEL_Q20_PHASED_WORK_DIR}/phased_timeline_always_rebuild.csv"
HYBRID_CSV="${ACCEL_Q20_PHASED_WORK_DIR}/phased_timeline.csv"

if [[ -f "${HYBRID_CSV}" && -f "${INC_CSV}" && -f "${REB_CSV}" ]]; then
  echo "=== replotted phase bands with measured baselines ===" >&2
  python3 "${ROOT}/scripts/plot_q20_phase_bands_styled.py" \
    --csv "${HYBRID_CSV}" \
    --always-incremental-csv "${INC_CSV}" \
    --always-rebuild-csv "${REB_CSV}" \
    --out-dir "${ACCEL_Q20_PHASED_WORK_DIR}/plots"
else
  echo "skip replot (need hybrid + both baseline CSVs in ${ACCEL_Q20_PHASED_WORK_DIR})" >&2
  echo "  hybrid: ${HYBRID_CSV} ($([ -f "${HYBRID_CSV}" ] && echo ok || echo missing))" >&2
  echo "  incremental: ${INC_CSV} ($([ -f "${INC_CSV}" ] && echo ok || echo missing))" >&2
  echo "  rebuild: ${REB_CSV} ($([ -f "${REB_CSV}" ] && echo ok || echo missing))" >&2
fi

echo "done." >&2
