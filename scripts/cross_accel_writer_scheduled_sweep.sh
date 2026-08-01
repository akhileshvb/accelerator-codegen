#!/usr/bin/env bash
# Cross-accelerator sweep: single writer with aux_bench-aligned write schedule (0/1/10/50%),
# concurrent readers each on their own query. Writer round-robins invalidating UPDATEs across
# the query mix (or set ACCEL_CROSS_ACCEL_WRITER_TARGET for targeted writes).
#
# Fairness:
# - Readers are not throttled at write boundaries (no reader_budget).
# - Aggregate reader QPS is capped equally at every write level so the writer can hit schedule.
# - Aux state is built in every benchmark process (registry is not shared across processes).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-release"
BIN="${BUILD}/benchmark_aux_cross_accelerator"
DB="${DB_PATH:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
OUT="${OUT:-${BUILD}/cross_accel_writer_scheduled_sweep.csv}"
LOG_DIR="${BUILD}/cross_accel_writer_scheduled_logs"
DURATION="${ACCEL_CROSS_ACCEL_DURATION_SEC:-30}"
READERS="${ACCEL_CROSS_ACCEL_READERS:-6}"
QUERIES="${ACCEL_CROSS_ACCEL_QUERIES:-1,3,6,10,12,14}"
FREQUENCIES="${ACCEL_CROSS_ACCEL_WRITER_SCHEDULED_FREQUENCIES:-1,10,50,0}"
WRITER_TARGET="${ACCEL_CROSS_ACCEL_WRITER_TARGET:-round_robin}"
AGGREGATE_QPS="${ACCEL_CROSS_ACCEL_AGGREGATE_READER_QPS:-auto}"
CAL_DURATION="${ACCEL_CROSS_ACCEL_CALIBRATION_SEC:-15}"

mkdir -p "$LOG_DIR"

if [[ ! -x "$BIN" ]]; then
  cmake --build "$BUILD" --target benchmark_aux_cross_accelerator -j"$(nproc)"
fi

if [[ "$AGGREGATE_QPS" == "auto" ]]; then
  echo "=== calibrating aggregate reader QPS for ~50% effective writes ==="
  AGGREGATE_QPS="$(python3 "${ROOT}/scripts/calibrate_aggregate_reader_qps.py" \
    --bin "$BIN" \
    --db "$DB" \
    --duration-sec "$CAL_DURATION" \
    --readers "$READERS" \
    --queries "$QUERIES" \
    --writer-target "$WRITER_TARGET")"
  echo "using ACCEL_CROSS_ACCEL_AGGREGATE_READER_QPS=${AGGREGATE_QPS}"
fi

target_label="${WRITER_TARGET}"
target_label="${target_label//_/-}"

echo "write_freq_sweep_label,writer_style,writer_target,readers,scheduled_write_pct,effective_write_pct,accel_writer_commits,total_reader_queries,workload_speedup,aggregate_reader_qps,query_num,accel_p50_us,duckdb_p50_us,speedup_p50" >"$OUT"

IFS=',' read -r -a freq_list <<< "${FREQUENCIES}"
for freq in "${freq_list[@]}"; do
  label="${freq}%-writer-${target_label}"
  log="${LOG_DIR}/${freq}pct_${target_label}.log"
  echo "=== writer-scheduled ${freq}% target=${WRITER_TARGET} readers=${READERS} aggregate_qps=${AGGREGATE_QPS} ==="

  env_args=(
    "ACCEL_CROSS_ACCEL_QUERIES=${QUERIES}"
    "ACCEL_CROSS_ACCEL_READERS=${READERS}"
    "ACCEL_CROSS_ACCEL_DURATION_SEC=${DURATION}"
    "ACCEL_CROSS_ACCEL_WRITER_SCHEDULED_WRITE_PCT=${freq}"
    "ACCEL_CROSS_ACCEL_WRITER_TARGET=${WRITER_TARGET}"
    "ACCEL_CROSS_ACCEL_AGGREGATE_READER_QPS=${AGGREGATE_QPS}"
    "ACCEL_CROSS_ACCEL_READER_PACE_US=0"
    "ACCEL_CROSS_ACCEL_DISABLE_WRITER=1"
    "ACCEL_CROSS_ACCEL_SKIP_CORRECTNESS=1"
    "ACCEL_CROSS_ACCEL_SKIP_BUILD=0"
  )

  env "${env_args[@]}" "$BIN" "$DB" isolation 2>&1 | tee "$log" >/dev/null

  total_reader=$(awk -F, '/^isolation,/ && $12 ~ /^[0-9]+$/ {sum += $13} END {print sum+0}' "$log")
  workload_speedup=$(awk -F, '/^isolation,/ && $12 ~ /^[0-9]+$/ {print $24; exit}' "$log")
  awk -F, -v label="$label" -v readers="$READERS" -v target="$WRITER_TARGET" -v total_reader="$total_reader" -v workload_speedup="$workload_speedup" -v aggregate_qps="$AGGREGATE_QPS" '
    /^isolation,/ && $12 ~ /^[0-9]+$/ {
      printf "%s,writer_scheduled,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
        label, target, readers, $10, $11, $7, total_reader, workload_speedup, aggregate_qps, $12, $15, $18, $19
    }
  ' "$log" >>"$OUT"
done

python3 "${ROOT}/scripts/summarize_writer_scheduled_sweep.py" "$OUT"
