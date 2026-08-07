#!/usr/bin/env bash
# Fair apples-to-apples aux performance sweep for DuckDB + Postgres on SF1/SF3 data.
#
# Measures each shared query under matched base vs aux conditions:
#   - read-only (write interval 0)
#   - write-interval sweep (default: 0.5s, 1s, 2s between probe writes)
#
# Reports per-run raw timings plus a summary with:
#   - read_speedup      = base read p50 / shared read p50
#   - workload_speedup  = (base read+write sum) / (shared read+write sum)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build}"
BIN="${BIN:-${BUILD}/benchmark_shared_duckdb_write_interval}"
DUCKDB_ROOT="${DUCKDB_ROOT:-${ROOT}/../accelerator-codegen-duckdb}"
DUCKDB_LIB="${DUCKDB_LIB:-${DUCKDB_ROOT}/build/release/src}"
if [[ -f "${DUCKDB_LIB}/libduckdb.so" ]]; then
  export LD_LIBRARY_PATH="${DUCKDB_LIB}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi
OUT="${OUT:-${ROOT}/benchmarks/aux_state/results/shared_aux_performance_sweep.csv}"
SUMMARY="${SUMMARY:-${ROOT}/benchmarks/aux_state/results/shared_aux_performance_summary.csv}"
LOG_DIR="${LOG_DIR:-${ROOT}/benchmarks/aux_state/results/shared_aux_performance_logs}"

TPCH_DUCKDB="${TPCH_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf1.duckdb}"
SEC_DUCKDB="${SEC_EDGAR_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
PG_TPCH_DB="${PG_TPCH_DB:-accel_aux_bench_q10}"
PG_SEC_DB="${PG_SEC_DB:-accel_sec_edgar_sf3}"
SKIP_TPCH_QUERIES="${SKIP_TPCH_QUERIES:-0}"
SKIP_SEC_QUERIES="${SKIP_SEC_QUERIES:-0}"
if [[ "$SKIP_TPCH_QUERIES" == "1" ]]; then
  TPCH_QUERIES=""
elif [[ -n "${TPCH_QUERIES:-}" ]]; then
  :
else
  TPCH_QUERIES="1 3 6 10 12 14"
fi
if [[ "$SKIP_SEC_QUERIES" == "1" ]]; then
  SEC_QUERIES=""
elif [[ -n "${SEC_QUERIES:-}" ]]; then
  :
else
  SEC_QUERIES="1 2 3 4 6 24"
fi

INTERVALS="${INTERVALS:-0 0.5 1 2}"
READS="${ACCEL_SHARED_SWEEP_READS:-8}"
READER_QPS="${ACCEL_SHARED_SWEEP_READER_QPS:-2.0}"
WARMUP="${ACCEL_SHARED_SWEEP_WARMUP:-1}"
SKIP_DIFF="${SKIP_DIFFERENTIAL:-0}"
SKIP_DUCKDB="${SKIP_DUCKDB:-0}"
SKIP_POSTGRES="${SKIP_POSTGRES:-0}"
SETUP_PG_SEC="${SETUP_PG_SEC:-auto}"

PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi

export PG_HOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PG_PORT:-5434}"
export PGPORT
export PGPASSWORD="${PGPASSWORD:-postgres}"
export ACCEL_SHARED_SWEEP_READS="$READS"
export ACCEL_SHARED_SWEEP_READER_QPS="$READER_QPS"
export ACCEL_SHARED_SWEEP_WARMUP="$WARMUP"
export ACCELERATOR_CODEGEN_CONFIG_DIR="${ACCELERATOR_CODEGEN_CONFIG_DIR:-${ROOT}/config}"

mkdir -p "$(dirname "$OUT")" "$LOG_DIR"

require_file() {
  if [[ ! -f "$1" ]]; then
    echo "missing required file: $1" >&2
    exit 1
  fi
}

pg_ready() {
  PGPASSWORD="${PGPASSWORD}" psql -h "$PG_HOST" -p "$PGPORT" -U postgres -d postgres -tAc 'select 1' >/dev/null 2>&1
}

pg_db_has_accel() {
  local db="$1"
  local fn="$2"
  PGPASSWORD="${PGPASSWORD}" psql -h "$PG_HOST" -p "$PGPORT" -U postgres -d "$db" -tAc \
    "SELECT 1 FROM pg_proc WHERE proname = '${fn}'" 2>/dev/null | grep -q 1
}

ensure_postgres_sec_sf3() {
  if pg_db_has_accel "$PG_SEC_DB" "accel_sec_edgar_q1_aux"; then
    return 0
  fi
  if [[ "$SETUP_PG_SEC" == "0" ]]; then
    echo "Postgres SEC SF3 database ${PG_SEC_DB} is missing aux functions and SETUP_PG_SEC=0" >&2
    exit 1
  fi
  echo "=== setting up Postgres SEC EDGAR SF3 database (${PG_SEC_DB}) ==="
  ACCEL_POSTGRES_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so}" \
    SEC_PG_DB="$PG_SEC_DB" SEC_EDGAR_DB="$SEC_DUCKDB" \
    bash "${ROOT}/scripts/setup_accel_postgres_sec_sf3_db.sh"
}

if [[ "$SKIP_DUCKDB" != "1" ]]; then
  require_file "$TPCH_DUCKDB"
  require_file "$SEC_DUCKDB"
  if [[ ! -x "$BIN" ]]; then
    echo "building ${BIN}..."
    cmake --build "$BUILD" --target benchmark_shared_duckdb_write_interval -j"$(nproc)"
  fi
  if ! ldd "$BIN" 2>/dev/null | grep -q 'libduckdb.so =>'; then
    if [[ ! -f "${DUCKDB_LIB}/libduckdb.so" ]]; then
      echo "missing libduckdb.so; build DuckDB in ${DUCKDB_ROOT} first" >&2
      exit 1
    fi
  fi
fi

if [[ "$SKIP_POSTGRES" != "1" ]]; then
  if ! pg_ready; then
    echo "PostgreSQL is not reachable at ${PG_HOST}:${PGPORT}" >&2
    exit 1
  fi
  if [[ -n "${TPCH_QUERIES// }" ]]; then
    if ! pg_db_has_accel "$PG_TPCH_DB" "accel_tpch_q1_aux"; then
      echo "Postgres TPC-H database missing aux functions: ${PG_TPCH_DB}" >&2
      echo "run: bash scripts/setup_postgres_aux_performance_dbs.sh" >&2
      exit 1
    fi
    bash "${ROOT}/scripts/ensure_accel_postgres_mutation_triggers.sh" "$PG_TPCH_DB"
  fi
  if [[ -n "${SEC_QUERIES// }" ]]; then
    ensure_postgres_sec_sf3
    bash "${ROOT}/scripts/ensure_accel_postgres_mutation_triggers.sh" "$PG_SEC_DB"
  fi
  if [[ -z "${TPCH_QUERIES// }" && -z "${SEC_QUERIES// }" ]]; then
    echo "no queries selected; set TPCH_QUERIES/SEC_QUERIES or unset SKIP_*_QUERIES" >&2
    exit 1
  fi
  if [[ "${ACCEL_PG_RESTART:-1}" == "1" ]] && docker ps -a --format '{{.Names}}' | grep -qx 'accel_pg_test'; then
    echo "=== restarting accel_pg_test to clear extension backend state ==="
    docker restart accel_pg_test >/dev/null
    for _ in $(seq 1 60); do
      pg_ready && break
      sleep 1
    done
  fi
fi

echo "engine,workload,query,write_interval_sec,mode,reads,writes,rows,read_p50_us,read_sum_us,write_p50_us,write_sum_us,build_us,workload_sum_us,workload_speedup_vs_base,workload_with_build_us,workload_with_build_speedup_vs_base" >"$OUT"

run_duckdb_query() {
  local workload="$1"
  local query="$2"
  local db="$3"
  local interval="$4"
  local log="${LOG_DIR}/duckdb_${workload}_q${query}_i${interval}.log"
  echo "=== duckdb ${workload} Q${query} interval=${interval}s ===" | tee "$log"
  "$BIN" "$workload" "$query" "$db" "$interval" 2>&1 | tee -a "$log" | awk -F, '/^(duckdb|postgres),/ { print }' >>"$OUT"
}

run_postgres_query() {
  local workload="$1"
  local query="$2"
  local db="$3"
  local log="${LOG_DIR}/postgres_${workload}_q${query}.log"
  echo "=== postgres ${workload} Q${query} intervals=${INTERVALS} ===" | tee "$log"
  local diff_flag=()
  if [[ "$SKIP_DIFF" == "1" ]]; then
    diff_flag+=(--skip-differential)
  fi
  (
    cd "${ROOT}/benchmarks/postgres"
    "$PYTHON" benchmark_shared_write_interval.py \
      --workload "$workload" \
      --query "$query" \
      --db "$db" \
      --reads "$READS" \
      --reader-qps "$READER_QPS" \
      --warmup "$WARMUP" \
      --output "$OUT" \
      --append \
      "${diff_flag[@]}" \
      $INTERVALS
  ) 2>&1 | tee -a "$log"
}

if [[ "$SKIP_DUCKDB" != "1" ]]; then
  for query in $TPCH_QUERIES; do
    for interval in $INTERVALS; do
      run_duckdb_query tpch "$query" "$TPCH_DUCKDB" "$interval"
    done
  done
  for query in $SEC_QUERIES; do
    for interval in $INTERVALS; do
      run_duckdb_query sec_edgar "$query" "$SEC_DUCKDB" "$interval"
    done
  done
fi

if [[ "$SKIP_POSTGRES" != "1" ]]; then
  for query in $TPCH_QUERIES; do
    run_postgres_query tpch "$query" "$PG_TPCH_DB"
  done
  for query in $SEC_QUERIES; do
    run_postgres_query sec_edgar "$query" "$PG_SEC_DB"
  done
fi

"$PYTHON" "${ROOT}/scripts/summarize_shared_interface_sweep.py" "$OUT" --summary "$SUMMARY"
echo "wrote raw sweep to $OUT"
echo "wrote summary to $SUMMARY"
