#!/usr/bin/env bash
# Fair SEC EDGAR parallel-aux sweep vs DuckDB SQL and Postgres SQL.
#
# Read timing for both engines is a full client Query/execute of the statement
# (parse + plan + execute + materialize):
#   base: catalog base_sql
#   aux:  SELECT * FROM accel_sec_edgar_qN_aux_parallel()
# Worker count is the same for SQL and aux (PRAGMA threads / max_parallel_workers
# per gather / accel.aux_worker_count / ACCEL_AUX_THREADS).
# Write intervals use CollectInvalidatingSharedProbes / catalog probes that
# change that query's base_sql (not an arbitrary first-row UPDATE).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build-release}"
BIN="${BIN:-${BUILD}/benchmark_shared_duckdb_write_interval_parallel}"
DUCKDB_ROOT="${DUCKDB_ROOT:-${ROOT}/../accelerator-codegen-duckdb}"
DUCKDB_LIB="${DUCKDB_LIB:-${DUCKDB_ROOT}/build/release/src}"
if [[ -f "${DUCKDB_LIB}/libduckdb.so" ]]; then
  export LD_LIBRARY_PATH="${DUCKDB_LIB}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi
OUT="${OUT:-${ROOT}/benchmarks/aux_state/results/parallel_sec_edgar_fair_sweep.csv}"
SUMMARY="${SUMMARY:-${ROOT}/benchmarks/aux_state/results/parallel_sec_edgar_fair_summary.csv}"
LOG_DIR="${LOG_DIR:-${ROOT}/benchmarks/aux_state/results/parallel_sec_edgar_fair_logs}"
[[ "$OUT" = /* ]] || OUT="${ROOT}/${OUT}"
[[ "$SUMMARY" = /* ]] || SUMMARY="${ROOT}/${SUMMARY}"
[[ "$LOG_DIR" = /* ]] || LOG_DIR="${ROOT}/${LOG_DIR}"

SEC_DUCKDB="${SEC_EDGAR_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
SEC_QUERIES="${SEC_QUERIES:-1 2 3 4 6 24}"
INTERVALS="${INTERVALS:-0 0.5 1 2}"
READS="${ACCEL_SHARED_SWEEP_READS:-8}"
READER_QPS="${ACCEL_SHARED_SWEEP_READER_QPS:-2.0}"
WARMUP="${ACCEL_SHARED_SWEEP_WARMUP:-1}"
THREADS="${ACCEL_BENCH_THREADS:-8}"
SKIP_POSTGRES="${SKIP_POSTGRES:-0}"
SKIP_DUCKDB="${SKIP_DUCKDB:-0}"
SKIP_DIFF="${SKIP_DIFFERENTIAL:-1}"
PG_SEC_DB="${PG_SEC_DB:-accel_sec_edgar_sf3}"

PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi

export ACCEL_SHARED_SWEEP_READS="$READS"
export ACCEL_SHARED_SWEEP_READER_QPS="$READER_QPS"
export ACCEL_SHARED_SWEEP_WARMUP="$WARMUP"
export ACCELERATOR_CODEGEN_CONFIG_DIR="${ACCELERATOR_CODEGEN_CONFIG_DIR:-${ROOT}/config}"
export ACCEL_BENCH_THREADS="$THREADS"
export ACCEL_AUX_THREADS="$THREADS"
export ACCEL_PG_MAX_PARALLEL_WORKERS_PER_GATHER="$THREADS"
export PG_HOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PG_PORT:-5434}"
export PGPORT
export PGPASSWORD="${PGPASSWORD:-postgres}"

mkdir -p "$(dirname "$OUT")" "$LOG_DIR"

if [[ "$SKIP_DUCKDB" != "1" ]]; then
  if [[ ! -x "$BIN" ]]; then
    echo "building ${BIN} (requires -DACCEL_ENABLE_PARALLEL_AUX=ON)..."
    cmake --build "$BUILD" --target benchmark_shared_duckdb_write_interval_parallel -j"$(nproc)"
  fi
  if [[ ! -f "$SEC_DUCKDB" ]]; then
    echo "missing SEC EDGAR DuckDB: $SEC_DUCKDB" >&2
    exit 1
  fi
fi

if [[ "$SKIP_DUCKDB" == "1" && -f "$OUT" ]]; then
  echo "appending to existing $OUT"
else
  echo "engine,workload,query,write_interval_sec,mode,reads,writes,rows,read_p50_us,read_sum_us,write_p50_us,write_sum_us,build_us,workload_sum_us,workload_speedup_vs_base,workload_with_build_us,workload_with_build_speedup_vs_base" > "$OUT"
fi
echo "fair sweep threads=${THREADS} intervals=${INTERVALS} queries=${SEC_QUERIES}" | tee "$LOG_DIR/meta.log"

if [[ "$SKIP_DUCKDB" != "1" ]]; then
  for q in $SEC_QUERIES; do
    for interval in $INTERVALS; do
      echo "=== duckdb parallel aux sec_edgar Q${q} interval=${interval} threads=${THREADS} ==="
      ACCEL_SHARED_SWEEP_OUTPUT="$OUT" ACCEL_SHARED_SWEEP_APPEND=1 \
        "$BIN" sec_edgar "$q" "$SEC_DUCKDB" "$interval" \
        | tee "$LOG_DIR/duckdb_sec_edgar_q${q}_${interval}.log"
    done
  done
fi

if [[ "$SKIP_POSTGRES" != "1" ]]; then
  if ! PGPASSWORD="${PGPASSWORD}" pg_isready -h "$PG_HOST" -p "$PGPORT" >/dev/null 2>&1; then
    echo "PostgreSQL is not reachable at ${PG_HOST}:${PGPORT}. Start it or set SKIP_POSTGRES=1." >&2
    exit 1
  fi
  echo "=== ensuring Postgres *_aux_parallel functions on ${PG_SEC_DB} ==="
  PG_HOST="$PG_HOST" PG_PORT="$PGPORT" PG_SEC_DB="$PG_SEC_DB" \
    bash "${ROOT}/scripts/ensure_postgres_sec_edgar_aux_parallel.sh"
  diff_flag=()
  if [[ "$SKIP_DIFF" == "1" ]]; then
    diff_flag+=(--skip-differential)
  fi
  for q in $SEC_QUERIES; do
    echo "=== postgres parallel aux sec_edgar Q${q} threads=${THREADS} ==="
    (cd "${ROOT}/benchmarks/postgres" && "$PYTHON" benchmark_shared_write_interval_parallel.py \
      --workload sec_edgar --query "$q" --db "$PG_SEC_DB" \
      --reads "$READS" --reader-qps "$READER_QPS" --warmup "$WARMUP" \
      --workers "$THREADS" \
      --output "$OUT" --append \
      "${diff_flag[@]}" \
      $INTERVALS) | tee "$LOG_DIR/postgres_sec_edgar_q${q}.log"
  done
fi

if [[ -f "${ROOT}/scripts/summarize_shared_interface_sweep.py" ]]; then
  "$PYTHON" "${ROOT}/scripts/summarize_shared_interface_sweep.py" "$OUT" --summary "$SUMMARY" || true
fi
echo "wrote $OUT"
