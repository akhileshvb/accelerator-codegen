#!/usr/bin/env bash
# TPC-H Verus accelerators at SF20: per query, read-only latency (query vs
# accelerator, per parameter tuple) and read/write mixes with refresh-style
# writes, on the verified Verus store: STORE=verus_global (default: one store,
# one lock, for every accelerator of the database; scripts/gen_verus_world.py)
# or STORE=verus (one store per accelerator).
#
# Writes (VERUS_KIT_WRITE_MODE, see eval/tests/verus_kit/verus_kit_runner.cpp):
#   auto (default)  rf (TPC-H RF1/RF2 mini-chunks on orders + lineitem) when the
#                   query reads orders or lineitem, else rf_dim (parts and
#                   suppliers with their partsupp rows): Q2, Q11, Q16
#   rf | rf_dim | txns  force one mode (txns: each query's own bench transactions)
#
# Each query runs on a fresh copy of the database (the runner modifies it), with
# its kit's SF20 cost model (accelerators/tpch/qN/config/verus/qN_aux_cost_model_sf20.json, from
# scripts/calibrate_verus_sf20_cost_models.sh) steering maintenance.
#
# Usage:
#   bash scripts/run_verus_sf20_sweep.sh                               # all 22
#   QUERIES="1 3" OPS=20 REPS=1 bash scripts/run_verus_sf20_sweep.sh   # smoke
# Env: DB, WORK, OUT, QUERIES, REPS (3), MIXES (0.01,0.05,0.1), OPS (100),
#      RF_CHUNK (1), WRITE_MODE (auto), STORE (verus_global), RUNNER, QUERY_TIMEOUT_SEC (10800).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB="${DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf20.duckdb}"
WORK="${WORK:-/flash1/akhilvb/tpch/work}"
OUT="${OUT:-${ROOT}/eval/bench/verus/results/sf20_$(date +%Y%m%d_%H%M%S)}"
QUERIES="${QUERIES:-$(seq -s ' ' 1 22)}"
REPS="${REPS:-3}"
MIXES="${MIXES:-0.01,0.05,0.1}"
OPS="${OPS:-100}"
RUNNER="${RUNNER:-${ROOT}/build-verus-duckdb/verus_kit_runner}"
QUERY_TIMEOUT_SEC="${QUERY_TIMEOUT_SEC:-10800}"
export ACCEL_AUX_STORE="${STORE:-verus_global}"
export VERUS_KIT_WRITE_MODE="${WRITE_MODE:-auto}"
export VERUS_KIT_RF_CHUNK="${RF_CHUNK:-1}"

[[ -f "${DB}" ]] || { echo "no database at ${DB} (scripts/setup_tpch_sf20_db.sh)" >&2; exit 1; }
[[ -x "${RUNNER}" ]] || { echo "no runner at ${RUNNER} (cmake --build build-verus-duckdb --target verus_kit_runner)" >&2; exit 1; }
mkdir -p "${WORK}" "${OUT}"
echo "db=${DB} out=${OUT} queries=[${QUERIES}] reps=${REPS} mixes=${MIXES} ops=${OPS} write_mode=${VERUS_KIT_WRITE_MODE} rf_chunk=${VERUS_KIT_RF_CHUNK} store=${ACCEL_AUX_STORE}"

for q in ${QUERIES}; do
  spec="${ROOT}/accelerators/tpch/q${q}/tests/workload"
  copy="${WORK}/sweep_q${q}.duckdb"
  # each kit's own SF20 cost model (scripts/calibrate_verus_sf20_cost_models.sh)
  model="${ROOT}/accelerators/tpch/q${q}/config/verus/q${q}_aux_cost_model_sf20.json"
  [[ -f "${model}" ]] || echo "  warning: no calibrated cost model ${model} (the policy uses defaults)"
  export ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL="${model}"
  echo "=== q${q} $(date +%T)"
  cp "${DB}" "${copy}"
  if timeout "${QUERY_TIMEOUT_SEC}" "${RUNNER}" bench "${copy}" "tpch_q${q}_vgen" "${spec}" "${REPS}" "${MIXES}" "${OPS}" \
      > "${OUT}/q${q}.json" 2> "${OUT}/q${q}.log"; then
    python3 "${ROOT}/scripts/verus_sweep_summary.py" "${OUT}" "q${q}"
  else
    echo "  FAILED (exit $?): see ${OUT}/q${q}.log"
  fi
  rm -f "${copy}" "${copy}.wal"
done

python3 "${ROOT}/scripts/verus_sweep_summary.py" "${OUT}"
