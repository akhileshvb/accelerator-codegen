#!/usr/bin/env bash
# SF1 PostgreSQL aux correctness for TPC-H: cold, incremental (invalidating
# point writes), and RF chunk differentials.
#
# Prerequisites:
#   bash scripts/start_accel_pg_test.sh
#   bash scripts/setup_postgres_aux_performance_dbs.sh   # or setup_accel_postgres_tpch_test_db.sh
#
# Usage:
#   bash scripts/validate_postgres_tpch_sf1_differential.sh
#   TPCH_PG_DB=accel_tpch_test bash scripts/validate_postgres_tpch_sf1_differential.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_DIR="${ROOT}/benchmarks/postgres"
TPCH_DB="${TPCH_PG_DB:-accel_aux_bench_q10}"
TEST_PY="${PG_DIR}/test_postgres_shared_differential.py"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi

export PG_HOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PG_PORT:-5434}"
export PGPORT
export PGPASSWORD="${PGPASSWORD:-postgres}"

if ! "$PYTHON" -c "import psycopg2" >/dev/null 2>&1; then
  echo "missing psycopg2; install with: pip install psycopg2-binary" >&2
  exit 1
fi

if [[ "${ACCEL_PG_RESTART:-1}" == "1" ]] && docker ps -a --format '{{.Names}}' | grep -qx 'accel_pg_test'; then
  echo "=== restarting accel_pg_test to clear extension backend state ==="
  docker restart accel_pg_test >/dev/null
  for _ in $(seq 1 60); do
    if PGPASSWORD="${PGPASSWORD}" psql -h "$PG_HOST" -p "$PGPORT" -U postgres -d postgres -tAc 'select 1' >/dev/null 2>&1; then
      break
    fi
    sleep 1
  done
fi

if ! PGPASSWORD="${PGPASSWORD}" psql -h "$PG_HOST" -p "$PGPORT" -U postgres -d "$TPCH_DB" -tAc "SELECT 1 FROM pg_proc WHERE proname = 'accel_tpch_q1_aux'" >/dev/null 2>&1; then
  echo "TPC-H SF1 database missing accel functions: $TPCH_DB" >&2
  echo "run: bash scripts/setup_postgres_aux_performance_dbs.sh" >&2
  exit 1
fi

bash "${ROOT}/scripts/ensure_accel_postgres_mutation_triggers.sh" "$TPCH_DB"

failures=0
run_mode() {
  local mode="$1"
  echo "=== postgres tpch ${mode} differential (SF1 ${TPCH_DB}) ==="
  if (
    cd "$PG_DIR"
    "$PYTHON" "$TEST_PY" --workload tpch --all --db "$TPCH_DB" --mode "$mode"
  ); then
    echo "PASS ${mode}"
  else
    echo "FAIL ${mode}"
    failures=$((failures + 1))
  fi
}

run_mode cold
run_mode incremental
run_mode rf

if [[ "$failures" -ne 0 ]]; then
  echo "${failures} postgres TPC-H SF1 differential suite(s) failed" >&2
  exit 1
fi

echo "all postgres TPC-H SF1 cold + incremental + RF differentials passed"
