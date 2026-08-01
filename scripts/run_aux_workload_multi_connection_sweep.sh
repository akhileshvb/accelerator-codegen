#!/usr/bin/env bash
# Per-query multi-connection workload sweep (aux_bench semantics + real concurrent readers/writer).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-release"
BIN="${BUILD}/benchmark_aux_workload_multi_connection"
DB="${DB_PATH:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
OUT="${OUT:-${BUILD}/aux_workload_multi_connection_sweep.csv}"
LOG_DIR="${LOG_DIR:-${BUILD}/aux_workload_multi_connection_logs}"
DURATION="${ACCEL_WORKLOAD_DURATION_SEC:-25}"
READERS="${ACCEL_WORKLOAD_READERS:-4}"
QUERIES="${ACCEL_WORKLOAD_QUERIES:-${QUERIES:-1,3,6,10,12,14}}"
FREQUENCIES="${ACCEL_WORKLOAD_FREQUENCIES:-0,1,10,50}"
CAL_DURATION="${ACCEL_WORKLOAD_CALIBRATION_SEC:-12}"
AGGREGATE_QPS="${ACCEL_WORKLOAD_AGGREGATE_READER_QPS:-}"
MIN_READER_QUERIES="${ACCEL_WORKLOAD_MIN_READER_QUERIES:-200}"
MERGE_IN="${MERGE_IN:-}"

mkdir -p "$LOG_DIR"

if [[ ! -x "$BIN" ]]; then
  cmake --build "$BUILD" --target benchmark_aux_workload_multi_connection -j"$(nproc)"
fi

PARTIAL_OUT="${OUT}.partial"
echo "experiment,query_num,write_frequency_pct,effective_write_pct,readers,duration_sec,aggregate_reader_qps,reader_queries,baseline_writer_commits,accel_writer_commits,baseline_read_sum_us,accel_read_sum_us,baseline_write_us,accel_write_us,workload_speedup,read_speedup_p50,read_speedup_p95,baseline_read_p50_us,accel_read_p50_us,baseline_read_p95_us,accel_read_p95_us,accel_hit_rate" >"$PARTIAL_OUT"

IFS=',' read -r -a query_list <<< "${QUERIES}"
IFS=',' read -r -a freq_list <<< "${FREQUENCIES}"

for query in "${query_list[@]}"; do
  if [[ -z "$AGGREGATE_QPS" || "$AGGREGATE_QPS" == "auto" ]]; then
    echo "=== calibrating Q${query} aggregate reader QPS ==="
    qps="$(python3 "${ROOT}/scripts/calibrate_workload_aggregate_reader_qps.py" \
      --bin "$BIN" --db "$DB" --query "$query" --duration-sec "$CAL_DURATION" --readers "$READERS")"
  else
    qps="$AGGREGATE_QPS"
  fi

  run_duration="$DURATION"
  min_duration="$(python3 - <<PY
import math
qps = float("${qps}")
min_q = int("${MIN_READER_QUERIES}")
base = int("${DURATION}")
print(max(base, int(math.ceil(min_q / max(qps, 0.001)))))
PY
)"
  run_duration="$min_duration"
  qps="$(python3 - <<PY
import math
qps = float("${qps}")
duration = int("${run_duration}")
print(max(qps, 100.0 / max(duration, 1)))
PY
)"
  echo "Q${query}: aggregate_reader_qps=${qps} duration_sec=${run_duration}"

  for freq in "${freq_list[@]}"; do
    log="${LOG_DIR}/q${query}_${freq}pct.log"
    echo "=== Q${query} write ${freq}% aggregate_qps=${qps} duration=${run_duration} ==="
    env_args=(
      "ACCEL_WORKLOAD_QUERY=${query}"
      "ACCEL_WORKLOAD_READERS=${READERS}"
      "ACCEL_WORKLOAD_DURATION_SEC=${run_duration}"
      "ACCEL_WORKLOAD_WRITE_PCT=${freq}"
      "ACCEL_WORKLOAD_AGGREGATE_READER_QPS=${qps}"
      "ACCEL_WORKLOAD_SKIP_CORRECTNESS=1"
      "ACCEL_WORKLOAD_SKIP_BUILD=0"
      "ACCEL_WORKLOAD_WARMUP=5"
    )
    env "${env_args[@]}" "$BIN" "$query" "$DB" 2>&1 | tee "$log" >/dev/null
    awk -F, -v q="$query" '
      /^[0-9]+,/ && $2 ~ /^[0-9]+$/ {
        printf "per_query_workload,%s,%s\n", q, $0
      }
    ' "$log" >>"$PARTIAL_OUT"
  done
done

if [[ -n "$MERGE_IN" && -f "$MERGE_IN" ]]; then
  merge_base="$(mktemp)"
  cp "$MERGE_IN" "$merge_base"
  python3 "${ROOT}/scripts/merge_workload_sweep_csv.py" --base "$merge_base" --update "$PARTIAL_OUT" --out "$OUT"
  rm -f "$merge_base" "$PARTIAL_OUT"
else
  mv "$PARTIAL_OUT" "$OUT"
fi

python3 "${ROOT}/scripts/summarize_aux_workload_multi_connection_sweep.py" "$OUT"
