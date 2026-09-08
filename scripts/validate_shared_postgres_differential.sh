#!/usr/bin/env bash
# Cold + incremental differential checks for all shared PostgreSQL queries.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_DIR="${ROOT}/benchmarks/postgres"
TPCH_DB="${TPCH_PG_DB:-accel_aux_bench_q10}"
SEC_DB="${SEC_PG_DB:-accel_sec_edgar_test}"
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

if ! PGPASSWORD="${PGPASSWORD}" psql -h "$PG_HOST" -p "$PGPORT" -U postgres -d postgres -tAc 'select 1' >/dev/null 2>&1; then
  echo "PostgreSQL is not reachable at ${PG_HOST}:${PGPORT}" >&2
  exit 1
fi
if ! PGPASSWORD="${PGPASSWORD}" psql -h "$PG_HOST" -p "$PGPORT" -U postgres -d "$TPCH_DB" -tAc "SELECT 1 FROM pg_proc WHERE proname = 'accel_tpch_q1_aux'" >/dev/null 2>&1; then
  echo "TPC-H benchmark database missing accel functions: $TPCH_DB" >&2
  exit 1
fi

if ! PGPASSWORD="${PGPASSWORD}" psql -h "$PG_HOST" -p "$PGPORT" -U postgres -d "$SEC_DB" -tAc "SELECT 1 FROM pg_proc WHERE proname = 'accel_sec_edgar_q1_aux'" >/dev/null 2>&1; then
  echo "SEC test database not ready; running setup script..."
  bash "${ROOT}/scripts/setup_accel_postgres_sec_test_db.sh"
fi

bash "${ROOT}/scripts/ensure_accel_postgres_mutation_triggers.sh" "$TPCH_DB"
bash "${ROOT}/scripts/ensure_accel_postgres_mutation_triggers.sh" "$SEC_DB"

failures=0
run_suite() {
  local workload="$1"
  local db="$2"
  local mode="$3"
  echo "=== postgres ${workload} ${mode} differential (all queries) ==="
  if (
    cd "$PG_DIR"
    "$PYTHON" "$TEST_PY" --workload "$workload" --all --db "$db" --mode "$mode"
  ); then
    echo "PASS"
  else
    echo "FAIL"
    failures=$((failures + 1))
  fi
}

run_suite tpch "$TPCH_DB" cold
run_suite sec_edgar "$SEC_DB" cold
run_suite tpch "$TPCH_DB" incremental
run_suite sec_edgar "$SEC_DB" incremental

if [[ "$failures" -ne 0 ]]; then
  echo "${failures} postgres differential suite(s) failed" >&2
  exit 1
fi

echo "all postgres TPC-H and SEC-EDGAR cold and incremental differential suites passed"
