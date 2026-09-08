#!/usr/bin/env bash
# Postgres TPC-H SF20 RF mini-chunk write-interval sweep (single-threaded).
#
# RF skips Q2/Q11/Q16 (part/partsupp/supplier; RF1/RF2 never invalidate them).
#
# Timing (same for base SQL and accel SRF): read_p50/read_sum are EXPLAIN ANALYZE
# Execution Time (server compute, GenDB-style: exclude client output/fetch).
# Writes stay wall-clock. Gather/aux workers are pinned to 0 (single-threaded).
#
# Prerequisites:
#   bash scripts/start_accel_pg_test.sh
#   bash scripts/setup_postgres_aux_performance_dbs.sh          # SF1 for diffs
#   bash scripts/setup_accel_postgres_tpch_sf20_db.sh           # SF20 for sweep
#
# Usage:
#   bash scripts/run_postgres_tpch_sf20_rf_sweep.sh
#   SKIP_SF1_DIFF=1 QUERIES="1 3 6 14" INTERVALS="0 60 30" DURATION_SEC=120 \
#     bash scripts/run_postgres_tpch_sf20_rf_sweep.sh
#   ACCEL_TPCH_RF_CHUNK_ORDERS=30000 DURATION_SEC=600 INTERVALS="0 300 120 60" \
#     bash scripts/run_postgres_tpch_sf20_rf_sweep.sh   # official-size RF sets
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_DIR="${ROOT}/benchmarks/postgres"
OUT_DIR="${OUT_DIR:-${ROOT}/benchmarks/postgres/results_sf20_rf}"
# Python runs after `cd $PG_DIR`; keep OUT/LOG absolute so relative env paths work.
if [[ "${OUT_DIR}" != /* ]]; then
  OUT_DIR="${ROOT}/${OUT_DIR}"
fi
LOG="${LOG:-${OUT_DIR}/postgres_tpch_sf20_rf_sweep.log}"
OUT="${OUT:-${OUT_DIR}/postgres_tpch_sf20_rf_sweep.csv}"
if [[ "${LOG}" != /* ]]; then
  LOG="${ROOT}/${LOG}"
fi
if [[ "${OUT}" != /* ]]; then
  OUT="${ROOT}/${OUT}"
fi
DB="${TPCH_PG_DB:-accel_tpch_sf20}"
SF1_DB="${TPCH_SF1_PG_DB:-accel_aux_bench_q10}"
# All TPC-H aux queries RF1/RF2 can invalidate (skip Q2, Q11, Q16).
QUERIES="${QUERIES:-1 3 4 5 6 7 8 9 10 12 13 14 15 17 18 19 20 21 22}"
INTERVALS="${INTERVALS:-0 60 30 10 5 2 1}"
DURATION_SEC="${DURATION_SEC:-120}"
MIN_WRITES="${ACCEL_AUX_BENCH_MIN_WRITES:-5}"
RF_CHUNK_ORDERS="${ACCEL_TPCH_RF_CHUNK_ORDERS:-1}"
WARMUP="${ACCEL_BENCH_WARMUP:-2}"
SKIP_DIFF="${SKIP_DIFFERENTIAL:-1}"
SKIP_SF1_DIFF="${SKIP_SF1_DIFF:-0}"

export PG_HOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PG_PORT:-5434}"
export PGPORT
export PGPASSWORD="${PGPASSWORD:-postgres}"
# Force single-threaded gather / aux workers (GenDB comparison is ST).
export ACCEL_BENCH_THREADS="${ACCEL_BENCH_THREADS:-0}"
export ACCEL_AUX_THREADS="${ACCEL_AUX_THREADS:-0}"
export ACCEL_PG_MAX_PARALLEL_WORKERS_PER_GATHER="${ACCEL_PG_MAX_PARALLEL_WORKERS_PER_GATHER:-0}"

PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi

mkdir -p "$OUT_DIR"
: >"$LOG"

echo "=== Postgres TPC-H SF20 RF sweep (single-threaded) ===" | tee -a "$LOG"
echo "db=${DB} queries=${QUERIES} intervals=${INTERVALS}" | tee -a "$LOG"
echo "duration_sec=${DURATION_SEC} min_writes=${MIN_WRITES} rf_chunk_orders=${RF_CHUNK_ORDERS}" | tee -a "$LOG"
echo "ACCEL_BENCH_THREADS=${ACCEL_BENCH_THREADS}" | tee -a "$LOG"

if [[ "$SKIP_SF1_DIFF" != "1" ]]; then
  echo "=== SF1 invalidating-write differentials on ${SF1_DB} ===" | tee -a "$LOG"
  TPCH_PG_DB="$SF1_DB" ACCEL_PG_RESTART="${ACCEL_PG_RESTART:-1}" \
    bash "${ROOT}/scripts/validate_postgres_tpch_sf1_differential.sh" 2>&1 | tee -a "$LOG"
fi

if ! PGPASSWORD="$PGPASSWORD" psql -h "$PG_HOST" -p "$PGPORT" -U postgres -d "$DB" -tAc 'select 1' >/dev/null; then
  echo "ERROR: cannot connect to ${DB} at ${PG_HOST}:${PGPORT}" >&2
  echo "run: bash scripts/setup_accel_postgres_tpch_sf20_db.sh" >&2
  exit 1
fi

header_written=0
: >"$OUT"
for query in $QUERIES; do
  if [[ "$query" == "2" || "$query" == "11" || "$query" == "16" ]]; then
    echo "=== SKIP postgres tpch Q${query} (RF does not invalidate this query) ===" | tee -a "$LOG"
    continue
  fi
  echo "=== postgres tpch Q${query} ===" | tee -a "$LOG"
  extra=()
  if [[ "$SKIP_DIFF" == "1" ]]; then
    extra+=(--skip-differential)
  fi
  append=()
  if [[ "$header_written" -eq 1 ]]; then
    append+=(--append)
  fi
  (
    cd "$PG_DIR"
    "$PYTHON" benchmark_shared_write_interval.py \
      --workload tpch --query "$query" --db "$DB" \
      --write-mode rf --rf-chunk-orders "$RF_CHUNK_ORDERS" \
      --duration-sec "$DURATION_SEC" --min-writes "$MIN_WRITES" --warmup "$WARMUP" \
      --output "$OUT" \
      "${append[@]}" \
      "${extra[@]}" \
      $INTERVALS
  ) 2>&1 | tee -a "$LOG"
  header_written=1
done

echo "=== complete; csv=${OUT} log=${LOG} ===" | tee -a "$LOG"
