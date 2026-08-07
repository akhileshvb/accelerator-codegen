#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build-release}"
BIN="${BIN:-${BUILD}/benchmark_shared_duckdb_write_interval}"
OUT="${OUT:-${ROOT}/benchmarks/aux_state/results/shared_write_interval_sweep.csv}"
TPCH_DB="${TPCH_DB:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
SEC_EDGAR_DB="${SEC_EDGAR_DB:-/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
PG_TPCH_DB="${PG_TPCH_DB:-tpch_sf1}"
PG_SEC_EDGAR_DB="${PG_SEC_EDGAR_DB:-sec_edgar_sf3}"
INTERVALS="${INTERVALS:-0 0.5 1 2}"

mkdir -p "$(dirname "$OUT")"

if [[ ! -x "$BIN" ]]; then
  echo "missing benchmark binary: $BIN" >&2
  exit 1
fi

echo "engine,query,write_interval_sec,mode,reads,writes,rows,read_p50_us,read_sum_us,write_p50_us,write_sum_us,workload_sum_us,workload_speedup_vs_base" >"$OUT"

run_duckdb_sweep() {
  local workload="$1"
  local db="$2"
  shift 2
  local queries=("$@")
  for query in "${queries[@]}"; do
    for interval in $INTERVALS; do
      echo "=== duckdb ${workload} Q${query} interval=${interval}s ==="
      ACCEL_SHARED_SWEEP_OUTPUT="$OUT" "$BIN" "$workload" "$query" "$db" "$interval"
    done
  done
}

run_postgres_sweep() {
  local workload="$1"
  local db="$2"
  shift 2
  local queries=("$@")
  for query in "${queries[@]}"; do
  echo "=== postgres ${workload} Q${query} ==="
    python3 "${ROOT}/benchmarks/postgres/benchmark_shared_write_interval.py" \
      --workload "$workload" --query "$query" --db "$db" \
      --output "$OUT" $INTERVALS
  done
}

run_duckdb_sweep tpch "$TPCH_DB" 1 3 6 10 12 14
run_duckdb_sweep sec_edgar "$SEC_EDGAR_DB" 1 2 3 4 6 24

run_postgres_sweep tpch "$PG_TPCH_DB" 1 3 6 10 12 14
run_postgres_sweep sec_edgar "$PG_SEC_EDGAR_DB" 1 2 3 4 6 24

echo "wrote sweep results to $OUT"
