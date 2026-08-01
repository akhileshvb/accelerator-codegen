#!/usr/bin/env bash
# Time-based write interval sweep: invalidating UPDATE every N seconds (wall clock).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-release"
BIN="${BUILD}/benchmark_aux_workload_multi_connection"
SOURCE_DB="${DB_PATH:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
OUT="${OUT:-${BUILD}/concurrent_experiments/aux_workload_write_interval_sweep.csv}"
LOG_DIR="${LOG_DIR:-${BUILD}/concurrent_experiments/aux_workload_write_interval_logs}"
WORK_DB_DIR="${ACCEL_WORKLOAD_WORK_DB_DIR:-${BUILD}/concurrent_experiments/aux_workload_write_interval_dbs}"
READERS="${ACCEL_WORKLOAD_READERS:-4}"
QUERIES="${ACCEL_WORKLOAD_QUERIES:-1,6,12,14}"
# Wall-clock intervals (seconds). 0 = no writes.
INTERVALS="${ACCEL_WORKLOAD_WRITE_INTERVALS:-0,60,30,10,5,2,1}"
BASE_DURATION="${ACCEL_WORKLOAD_DURATION_SEC:-120}"
CAL_DURATION="${ACCEL_WORKLOAD_CALIBRATION_SEC:-12}"
MIN_WRITES="${ACCEL_WORKLOAD_MIN_WRITES:-30}"
AGGREGATE_QPS="${ACCEL_WORKLOAD_AGGREGATE_READER_QPS:-}"
MERGE_IN="${MERGE_IN:-}"
FRESH_DB="${ACCEL_WORKLOAD_FRESH_DB:-1}"

mkdir -p "$LOG_DIR" "$WORK_DB_DIR"

if [[ ! -x "$BIN" ]]; then
  cmake --build "$BUILD" --target benchmark_aux_workload_multi_connection -j"$(nproc)"
fi

PARTIAL_OUT="${OUT}.partial"
echo "experiment,query_num,write_interval_sec,writes_per_sec,effective_write_pct,readers,duration_sec,aggregate_reader_qps,reader_queries,baseline_writer_commits,accel_writer_commits,baseline_read_sum_us,accel_read_sum_us,baseline_write_us,accel_write_us,workload_speedup,read_speedup_p50,read_speedup_p95,baseline_read_p50_us,accel_read_p50_us,baseline_read_p95_us,accel_read_p95_us,accel_hit_rate" >"$PARTIAL_OUT"

IFS=',' read -r -a query_list <<< "${QUERIES}"
IFS=',' read -r -a interval_list <<< "${INTERVALS}"

run_duration_for_interval() {
  python3 - <<PY
import math
base = int("${BASE_DURATION}")
iv = float("${1}")
min_writes = int("${MIN_WRITES}")
if iv <= 0:
    print(base)
else:
    print(max(base, int(math.ceil(min_writes * iv))))
PY
}

for query in "${query_list[@]}"; do
  if [[ -z "$AGGREGATE_QPS" || "$AGGREGATE_QPS" == "auto" ]]; then
    echo "=== calibrating Q${query} aggregate reader QPS (no writes) ==="
    cal_db="$SOURCE_DB"
    if [[ "$FRESH_DB" == "1" ]]; then
      cal_db="${WORK_DB_DIR}/q${query}_cal.duckdb"
      cp -f "$SOURCE_DB" "$cal_db"
    fi
    qps="$(python3 "${ROOT}/scripts/calibrate_workload_aggregate_reader_qps.py" \
      --bin "$BIN" --db "$cal_db" --query "$query" --duration-sec "$CAL_DURATION" \
      --readers "$READERS" --write-pct 0 --mode read-throughput)"
  else
    qps="$AGGREGATE_QPS"
  fi
  echo "Q${query}: aggregate_reader_qps=${qps}"

  for interval in "${interval_list[@]}"; do
    run_duration="$(run_duration_for_interval "$interval")"
    log="${LOG_DIR}/q${query}_iv${interval}s.log"
    bench_db="$SOURCE_DB"
    if [[ "$FRESH_DB" == "1" ]]; then
      bench_db="${WORK_DB_DIR}/q${query}_iv${interval}s.duckdb"
      cp -f "$SOURCE_DB" "$bench_db"
    fi
    echo "=== Q${query} write_interval=${interval}s aggregate_qps=${qps} duration=${run_duration} fresh_db=${FRESH_DB} ==="
    env_args=(
      "ACCEL_WORKLOAD_QUERY=${query}"
      "ACCEL_WORKLOAD_READERS=${READERS}"
      "ACCEL_WORKLOAD_DURATION_SEC=${run_duration}"
      "ACCEL_WORKLOAD_WRITE_PCT=0"
      "ACCEL_WORKLOAD_AGGREGATE_READER_QPS=${qps}"
      "ACCEL_WORKLOAD_SKIP_CORRECTNESS=1"
      "ACCEL_WORKLOAD_SKIP_BUILD=0"
      "ACCEL_WORKLOAD_WARMUP=5"
    )
    if [[ "$interval" != "0" ]]; then
      env_args+=("ACCEL_WORKLOAD_WRITE_INTERVAL_SEC=${interval}")
    fi
    env "${env_args[@]}" "$BIN" "$query" "$bench_db" 2>&1 | tee "$log" >/dev/null
    awk -F, -v q="$query" '
      /^[0-9]+,/ && $2 ~ /^[0-9]+$/ {
        printf "write_interval,%s,%s\n", q, $0
      }
    ' "$log" >>"$PARTIAL_OUT"
  done
done

if [[ -n "$MERGE_IN" && -f "$MERGE_IN" ]]; then
  python3 "${ROOT}/scripts/merge_write_interval_sweep_csv.py" \
    --base "$MERGE_IN" --update "$PARTIAL_OUT" --out "$OUT"
  rm -f "$PARTIAL_OUT"
else
  mv "$PARTIAL_OUT" "$OUT"
fi

python3 "${ROOT}/scripts/summarize_aux_workload_write_interval_sweep.py" "$OUT"
python3 "${ROOT}/scripts/plot_aux_workload_write_interval_sweep.py" "$OUT" \
  --out-dir "${ROOT}/build-release/concurrent_experiments_report"
