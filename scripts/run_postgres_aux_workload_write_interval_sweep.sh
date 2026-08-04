#!/usr/bin/env bash
# PostgreSQL aux workload sweep: base PostgreSQL reference SQL vs Postgres-backed in-memory aux (Q10 / Q24).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build}"
BIN="${BUILD}/benchmark_postgres_aux_workload_multi_connection"
OUT="${OUT:-${BUILD}/concurrent_experiments/postgres_aux_workload_write_interval_sweep.csv}"
LOG_DIR="${LOG_DIR:-${BUILD}/concurrent_experiments/postgres_aux_workload_write_interval_logs}"
READERS="${ACCEL_WORKLOAD_READERS:-4}"
QUERIES="${ACCEL_WORKLOAD_QUERIES:-10,24}"
INTERVALS="${ACCEL_WORKLOAD_WRITE_INTERVALS:-0,60,30,10,5,2,1,0.5,0.25,0.1}"
BASE_DURATION="${ACCEL_WORKLOAD_DURATION_SEC:-60}"
CAL_DURATION="${ACCEL_WORKLOAD_CALIBRATION_SEC:-12}"
MIN_WRITES="${ACCEL_WORKLOAD_MIN_WRITES:-20}"
AGGREGATE_QPS="${ACCEL_WORKLOAD_AGGREGATE_READER_QPS:-}"
CONNINFO="${ACCEL_POSTGRES_CONNINFO:-dbname=accel_aux_bench host=localhost port=5434 user=postgres password=postgres}"
TPCH_DB="${TPCH_DB_PATH:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
SEC_DB="${SEC_EDGAR_DB_PATH:-/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
PGPASSWORD="${PGPASSWORD:-$(echo "$CONNINFO" | sed -n 's/.*password=\([^ ]*\).*/\1/p')}"
export PGPASSWORD

mkdir -p "$LOG_DIR" "$(dirname "$OUT")"

if [[ ! -x "$BIN" ]]; then
  cmake --build "$BUILD" --target benchmark_postgres_aux_workload_multi_connection -j"$(nproc)"
fi

setup_postgres_db() {
  local dbname="$1"
  local host port
  host="$(echo "$CONNINFO" | sed -n 's/.*host=\([^ ]*\).*/\1/p')"
  port="$(echo "$CONNINFO" | sed -n 's/.*port=\([^ ]*\).*/\1/p')"
  psql "host=${host} port=${port} user=postgres dbname=postgres" -v ON_ERROR_STOP=1 \
    -c "CREATE DATABASE ${dbname}" 2>/dev/null || true
}

query_conninfo() {
  local dbname="$1"
  local host port pass
  host="$(echo "$CONNINFO" | sed -n 's/.*host=\([^ ]*\).*/\1/p')"
  port="$(echo "$CONNINFO" | sed -n 's/.*port=\([^ ]*\).*/\1/p')"
  pass="$(echo "$CONNINFO" | sed -n 's/.*password=\([^ ]*\).*/\1/p')"
  if [[ -n "$pass" ]]; then
    echo "dbname=${dbname} host=${host} port=${port} user=postgres password=${pass}"
  else
    echo "dbname=${dbname} host=${host} port=${port} user=postgres"
  fi
}

postgres_data_ready() {
  local query="$1"
  local pg_db="$2"
  local host port
  host="$(echo "$CONNINFO" | sed -n 's/.*host=\([^ ]*\).*/\1/p')"
  port="$(echo "$CONNINFO" | sed -n 's/.*port=\([^ ]*\).*/\1/p')"
  if [[ "$query" == "10" ]]; then
    local count
    count="$(psql "host=${host} port=${port} user=postgres dbname=${pg_db}" -tAc "SELECT COUNT(*) FROM lineitem" 2>/dev/null || echo 0)"
    [[ "${count:-0}" -gt 1000000 ]]
    return
  fi
  local count
  count="$(psql "host=${host} port=${port} user=postgres dbname=${pg_db}" -tAc "SELECT COUNT(*) FROM num" 2>/dev/null || echo 0)"
  [[ "${count:-0}" -gt 10000 ]]
}

calibrate_postgres_reader_qps() {
  local query="$1"
  local duck_db="$2"
  local query_conninfo="$3"
  python3 - <<PY
import os, subprocess, sys
bin = "${BIN}"
query = "${query}"
duck_db = "${duck_db}"
duration = int("${CAL_DURATION}")
readers = int("${READERS}")
env = os.environ.copy()
env.update({
    "ACCEL_POSTGRES_CONNINFO": "${query_conninfo}",
    "ACCEL_POSTGRES_SKIP_SCAN_REFRESH": "1",
    "ACCEL_WORKLOAD_DURATION_SEC": str(duration),
    "ACCEL_WORKLOAD_READERS": str(readers),
    "ACCEL_WORKLOAD_WARMUP": "0",
})
proc = subprocess.run([bin, query, duck_db], env=env, capture_output=True, text=True)
out = proc.stdout + proc.stderr
for line in out.splitlines():
    if line.startswith("query_num,"):
        continue
    parts = line.split(",")
    if len(parts) < 9:
        continue
    try:
        int(parts[0])
        queries = int(float(parts[8]))
        qps = queries / duration if duration > 0 else 0.0
        print(f"{qps:.3f}")
        sys.exit(0)
    except ValueError:
        continue
print("50")
PY
}

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

PARTIAL_OUT="${OUT}.partial"
echo "experiment,query_num,write_interval_sec,writes_per_sec,effective_write_pct,readers,duration_sec,aggregate_reader_qps,reader_queries,baseline_writer_commits,accel_writer_commits,baseline_read_sum_us,accel_read_sum_us,baseline_write_us,accel_write_us,workload_speedup,read_speedup_p50,read_speedup_p95,baseline_read_p50_us,accel_read_p50_us,baseline_read_p95_us,accel_read_p95_us,accel_hit_rate" >"$PARTIAL_OUT"

IFS=',' read -r -a query_list <<< "${QUERIES}"
IFS=',' read -r -a interval_list <<< "${INTERVALS}"

for query in "${query_list[@]}"; do
  if [[ "$query" == "10" ]]; then
    DUCK_DB="$TPCH_DB"
    PG_DB="accel_aux_bench_q10"
  else
    DUCK_DB="$SEC_DB"
    PG_DB="accel_aux_bench_q24"
  fi

  setup_postgres_db "$PG_DB"
  QUERY_CONNINFO="$(query_conninfo "$PG_DB")"

  echo "=== loading Q${query} data into postgres (${PG_DB}) ==="
  if [[ "${ACCEL_POSTGRES_SKIP_LOAD:-0}" == "1" ]] || postgres_data_ready "$query" "$PG_DB"; then
    echo "Q${query}: postgres data already present, skipping load"
  else
    load_duck_db="$DUCK_DB"
    if [[ ! -f "${load_duck_db}.postgres_load_copy" ]]; then
      cp -f "$DUCK_DB" "${DUCK_DB}.postgres_load_copy"
    fi
    load_duck_db="${DUCK_DB}.postgres_load_copy"
    load_env=(ACCEL_POSTGRES_CONNINFO="$QUERY_CONNINFO" ACCEL_POSTGRES_LOAD_DATA=1)
    if [[ "$query" == "24" ]]; then
      load_env+=(ACCEL_POSTGRES_LOAD_NUM_LIMIT="${ACCEL_POSTGRES_LOAD_NUM_LIMIT:-500000}")
    fi
    env "${load_env[@]}" "$BIN" "$query" "$load_duck_db" 2>&1 | tee "${LOG_DIR}/q${query}_load.log"
  fi

  if [[ -z "$AGGREGATE_QPS" || "$AGGREGATE_QPS" == "auto" ]]; then
    echo "=== calibrating Q${query} aggregate reader QPS (postgres accel, no writes) ==="
    qps="$(calibrate_postgres_reader_qps "$query" "$DUCK_DB" "$QUERY_CONNINFO")"
    echo "Q${query}: calibrated aggregate_reader_qps=${qps}"
  else
    qps="$AGGREGATE_QPS"
  fi

  for interval in "${interval_list[@]}"; do
    run_duration="$(run_duration_for_interval "$interval")"
    log="${LOG_DIR}/q${query}_iv${interval}s.log"

    echo "=== Q${query} postgres_aux write_interval=${interval}s aggregate_qps=${qps} duration=${run_duration} ==="
    env_args=(
      "ACCEL_POSTGRES_CONNINFO=${QUERY_CONNINFO}"
      "ACCEL_POSTGRES_SKIP_SCAN_REFRESH=1"
      "ACCEL_WORKLOAD_READERS=${READERS}"
      "ACCEL_WORKLOAD_DURATION_SEC=${run_duration}"
      "ACCEL_WORKLOAD_AGGREGATE_READER_QPS=${qps}"
      "ACCEL_WORKLOAD_WARMUP=5"
    )
    if [[ "$interval" != "0" ]]; then
      env_args+=("ACCEL_WORKLOAD_WRITE_INTERVAL_SEC=${interval}")
    fi
    env "${env_args[@]}" "$BIN" "$query" "$DUCK_DB" 2>&1 | tee "$log" >/dev/null
    awk -F, -v q="$query" '
      /^[0-9]+,/ && $2 ~ /^[0-9.]+$/ {
        printf "write_interval,%s,%s\n", q, $0
      }
    ' "$log" >>"$PARTIAL_OUT"
  done
done

mv "$PARTIAL_OUT" "$OUT"
python3 "${ROOT}/scripts/summarize_aux_workload_write_interval_sweep.py" "$OUT"
python3 "${ROOT}/scripts/plot_postgres_aux_workload_write_interval_sweep.py" "$OUT" \
  --out-dir "${ROOT}/benchmarks/postgres/results"
