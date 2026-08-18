#!/usr/bin/env bash
# Postgres TPC-H SF20 RF mini-chunk write-interval sweep (point/RF via Python harness).
#
# Prerequisites:
#   bash scripts/start_accel_pg_test.sh
#   # build/install extension into container (see scripts/build_accel_postgres_extension.sh)
#   bash scripts/setup_accel_postgres_tpch_sf20_db.sh
#
# Usage:
#   bash scripts/run_postgres_tpch_sf20_rf_sweep.sh
#   QUERIES="1 3 6 14" INTERVALS="0 300 120 60" DURATION_SEC=120 \
#     bash scripts/run_postgres_tpch_sf20_rf_sweep.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_DIR="${ROOT}/benchmarks/postgres"
OUT_DIR="${OUT_DIR:-${ROOT}/benchmarks/postgres/results_sf20_rf}"
LOG="${LOG:-${OUT_DIR}/postgres_tpch_sf20_rf_sweep.log}"
OUT="${OUT:-${OUT_DIR}/postgres_tpch_sf20_rf_sweep.csv}"
DB="${TPCH_PG_DB:-accel_tpch_sf20}"
# Aux SRFs currently shipped for these TPC-H queries only.
QUERIES="${QUERIES:-1 3 6 10 12 14}"
INTERVALS="${INTERVALS:-0 300 120 60}"
DURATION_SEC="${DURATION_SEC:-600}"
MIN_WRITES="${ACCEL_AUX_BENCH_MIN_WRITES:-5}"
RF_CHUNK_ORDERS="${ACCEL_TPCH_RF_CHUNK_ORDERS:-30000}"
WARMUP="${ACCEL_BENCH_WARMUP:-2}"
SKIP_DIFF="${SKIP_DIFFERENTIAL:-1}"

export PG_HOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PG_PORT:-5434}"
export PGPORT
export PGPASSWORD="${PGPASSWORD:-postgres}"

PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi

mkdir -p "$OUT_DIR"
: >"$LOG"
: >"$OUT"

echo "=== Postgres TPC-H SF20 RF sweep ===" | tee -a "$LOG"
echo "db=${DB} queries=${QUERIES} intervals=${INTERVALS}" | tee -a "$LOG"
echo "duration_sec=${DURATION_SEC} min_writes=${MIN_WRITES} rf_chunk_orders=${RF_CHUNK_ORDERS}" | tee -a "$LOG"

if ! PGPASSWORD="$PGPASSWORD" psql -h "$PG_HOST" -p "$PGPORT" -U postgres -d "$DB" -tAc 'select 1' >/dev/null; then
  echo "ERROR: cannot connect to ${DB} at ${PG_HOST}:${PGPORT}" >&2
  echo "run: bash scripts/setup_accel_postgres_tpch_sf20_db.sh" >&2
  exit 1
fi

header_written=0
for query in $QUERIES; do
  echo "=== postgres tpch Q${query} ===" | tee -a "$LOG"
  args=(
    --workload tpch --query "$query" --db "$DB"
    --write-mode rf --rf-chunk-orders "$RF_CHUNK_ORDERS"
    --duration-sec "$DURATION_SEC" --min-writes "$MIN_WRITES" --warmup "$WARMUP"
    --output "$OUT" --append
  )
  if [[ "$SKIP_DIFF" == "1" ]]; then
    args+=(--skip-differential)
  fi
  if [[ "$header_written" -eq 0 ]]; then
    # First query: truncate + write header via non-append then append rest.
    (
      cd "$PG_DIR"
      "$PYTHON" benchmark_shared_write_interval.py \
        --workload tpch --query "$query" --db "$DB" \
        --write-mode rf --rf-chunk-orders "$RF_CHUNK_ORDERS" \
        --duration-sec "$DURATION_SEC" --min-writes "$MIN_WRITES" --warmup "$WARMUP" \
        --output "$OUT" \
        $( [[ "$SKIP_DIFF" == "1" ]] && echo --skip-differential ) \
        $INTERVALS
    ) 2>&1 | tee -a "$LOG"
    header_written=1
  else
    (
      cd "$PG_DIR"
      "$PYTHON" benchmark_shared_write_interval.py "${args[@]}" $INTERVALS
    ) 2>&1 | tee -a "$LOG"
  fi
done

echo "=== complete; csv=${OUT} log=${LOG} ===" | tee -a "$LOG"
