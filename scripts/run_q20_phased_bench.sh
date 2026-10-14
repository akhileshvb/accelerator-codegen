#!/usr/bin/env bash
# Run TPC-H Q20 phased cost-model experiment (phases A/B/C/D) and plot results.
#
# Policy selection (optional):
#   ACCEL_Q20_PHASED_POLICY=hybrid|always_incremental|always_rebuild
#     hybrid              — cost-model choice (default); writes phased_timeline.csv
#     always_incremental  — force ApplyDelta on every write
#     always_rebuild      — force RebuildAll on every write
#
# Or set ACCEL_FORCE_MAINTENANCE_ACTION=ApplyDelta|RebuildAll directly.
#
# SF20 example (adaptive):
#   ACCEL_TPCH_DB=/path/to/tpch_sf20.duckdb \
#   ACCEL_Q20_PHASED_WORK_DIR=build/q20_phased_sf20 \
#   ./scripts/run_q20_phased_bench.sh
#
# SF20 baselines (see also scripts/run_q20_phased_sf20_baselines.sh):
#   ACCEL_Q20_PHASED_POLICY=always_incremental ... ./scripts/run_q20_phased_bench.sh
#   ACCEL_Q20_PHASED_POLICY=always_rebuild ... ./scripts/run_q20_phased_bench.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
BENCH_BIN="${BUILD_DIR}/benchmark_tpch_q20_phased_eval"
SOURCE_DB="${ACCEL_TPCH_DB:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
WORK_DIR="${ACCEL_Q20_PHASED_WORK_DIR:-${BUILD_DIR}/q20_phased_sf1}"
COST_MODEL="${ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL:-${ROOT}/accelerators/tpch/q20/config/q20_aux_cost_model_phased.json}"
POLICY="${ACCEL_Q20_PHASED_POLICY:-hybrid}"
mkdir -p "${WORK_DIR}"

case "${POLICY}" in
  hybrid)
    FORCE_ACTION="${ACCEL_FORCE_MAINTENANCE_ACTION:-}"
    TIMELINE_NAME="phased_timeline.csv"
    SUMMARY_NAME="phased_summary.txt"
    ;;
  always_incremental)
    FORCE_ACTION="ApplyDelta"
    TIMELINE_NAME="phased_timeline_always_incremental.csv"
    SUMMARY_NAME="phased_summary_always_incremental.txt"
    ;;
  always_rebuild)
    FORCE_ACTION="RebuildAll"
    TIMELINE_NAME="phased_timeline_always_rebuild.csv"
    SUMMARY_NAME="phased_summary_always_rebuild.txt"
    ;;
  *)
    echo "unknown ACCEL_Q20_PHASED_POLICY=${POLICY} (use hybrid|always_incremental|always_rebuild)" >&2
    exit 1
    ;;
esac

# Direct FORCE env overrides policy mapping when set explicitly with hybrid.
if [[ -n "${ACCEL_FORCE_MAINTENANCE_ACTION:-}" && "${POLICY}" == "hybrid" ]]; then
  FORCE_ACTION="${ACCEL_FORCE_MAINTENANCE_ACTION}"
  case "${FORCE_ACTION}" in
    ApplyDelta) TIMELINE_NAME="phased_timeline_always_incremental.csv"; SUMMARY_NAME="phased_summary_always_incremental.txt" ;;
    RebuildAll) TIMELINE_NAME="phased_timeline_always_rebuild.csv"; SUMMARY_NAME="phased_summary_always_rebuild.txt" ;;
  esac
fi

if [[ ! -x "${BENCH_BIN}" ]]; then
  echo "missing benchmark binary: ${BENCH_BIN}" >&2
  echo "build with: cmake --build ${BUILD_DIR} --target benchmark_tpch_q20_phased_eval" >&2
  exit 1
fi

if [[ ! -f "${SOURCE_DB}" ]]; then
  echo "missing source DB: ${SOURCE_DB}" >&2
  exit 1
fi

bench_db="${WORK_DIR}/tpch_phased_${POLICY}.duckdb"
timeline="${WORK_DIR}/${TIMELINE_NAME}"
cp "${SOURCE_DB}" "${bench_db}"

echo "running Q20 phased bench policy=${POLICY} force=${FORCE_ACTION:-<cost-model>} cost_model=${COST_MODEL}" >&2
echo "  db_copy=${bench_db}" >&2
echo "  timeline=${timeline}" >&2

env_args=(
  "ACCEL_TPCH_DB=${bench_db}"
  "ACCEL_Q20_PHASED_TIMELINE_CSV=${timeline}"
  "ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL=${COST_MODEL}"
)
if [[ -n "${FORCE_ACTION}" ]]; then
  env_args+=("ACCEL_FORCE_MAINTENANCE_ACTION=${FORCE_ACTION}")
else
  # Ensure a prior shell export cannot leak into hybrid runs.
  env_args+=("ACCEL_FORCE_MAINTENANCE_ACTION=")
fi

(
  cd "${ROOT}"
  env "${env_args[@]}" "${BENCH_BIN}" > "${WORK_DIR}/${SUMMARY_NAME}"
)

# Only auto-plot the adaptive hybrid timeline; baselines are overlaid later.
if [[ "${POLICY}" == "hybrid" && -z "${FORCE_ACTION}" ]]; then
  python3 "${ROOT}/scripts/plot_q20_phased_bench.py" --csv "${timeline}" --out-dir "${WORK_DIR}/plots"
  python3 "${ROOT}/scripts/plot_q20_phase_bands_styled.py" --csv "${timeline}" --out-dir "${WORK_DIR}/plots"
fi

echo "wrote ${timeline}" >&2
echo "summary ${WORK_DIR}/${SUMMARY_NAME}" >&2
rm -f "${bench_db}"
