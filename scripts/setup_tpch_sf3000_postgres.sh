#!/usr/bin/env bash
# Load TPC-H SF3000 into Docker Postgres on /disk3 (aux vs base; no matview required).
#
# Prerequisites:
#   1. DuckDB SF3000 file (hours–days to generate):
#        SF=3000 MIN_DISK_GB=4000 bash scripts/setup_tpch_sf_db.sh
#   2. ~10–15 TB free on /disk3 for Postgres heap + indexes
#   3. Docker image postgres:17 and scripts/start_accel_pg_test.sh
#
# Usage:
#   bash scripts/setup_tpch_sf3000_postgres.sh
#
# Env overrides:
#   ACCEL_PG_DATA=/disk3/akhilvb_tpch/sf3000_pg_data
#   ACCEL_PG_SHARED_BUFFERS=64GB ACCEL_PG_SHM_SIZE=80g
#   TPCH_PG_DB=accel_tpch_sf3000
#   ACCEL_TPCH_SF3000_DB=/disk3/akhilvb_tpch/duckdb_data/tpch_sf3000.duckdb
#   SKIP_CONTAINER=1 SKIP_LOAD=1 SKIP_INDEXES=1 SKIP_EXTENSION=1
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_ROOT="${DUCKDB_ROOT:-${ROOT}/../accelerator-codegen-duckdb}"
SF3000_DUCKDB="${ACCEL_TPCH_SF3000_DB:-/disk3/akhilvb_tpch/duckdb_data/tpch_sf3000.duckdb}"
if [[ ! -f "${SF3000_DUCKDB}" && -f "${DUCKDB_ROOT}/tpch_sf3000.duckdb" ]]; then
  SF3000_DUCKDB="${DUCKDB_ROOT}/tpch_sf3000.duckdb"
fi
NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
PORT="${PG_PORT:-${PGPORT:-5434}}"
DB="${TPCH_PG_DB:-accel_tpch_sf3000}"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
[[ -x "${PYTHON}" ]] || PYTHON=python3

export ACCEL_PG_DATA="${ACCEL_PG_DATA:-/disk3/akhilvb_tpch/sf3000_pg_data}"
export ACCEL_PG_BUILD="${ACCEL_PG_BUILD:-/disk3/akhilvb_tpch/build}"
export ACCEL_PG_SHM_SIZE="${ACCEL_PG_SHM_SIZE:-80g}"
export ACCEL_PG_SHARED_BUFFERS="${ACCEL_PG_SHARED_BUFFERS:-64GB}"
export PGPASSWORD="${PGPASSWORD:-postgres}"
export PGHOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PORT}"
export PGUSER="${PGUSER:-postgres}"

MIN_DISK_GB="${MIN_DISK_GB:-8000}"
mkdir -p "${ACCEL_PG_DATA}" "${ACCEL_PG_BUILD}"
free_gb="$(df -BG "${ACCEL_PG_DATA}" | awk 'NR==2 {gsub(/G/,"",$4); print $4}')"
echo "setup_tpch_sf3000_postgres: data=${ACCEL_PG_DATA} free_gb=${free_gb} db=${DB}"
if [[ "${free_gb}" -lt "${MIN_DISK_GB}" ]]; then
  echo "ERROR: need >= ${MIN_DISK_GB} GB free on ${ACCEL_PG_DATA}, have ${free_gb} GB" >&2
  exit 1
fi
if [[ ! -f "${SF3000_DUCKDB}" ]]; then
  echo "ERROR: missing ${SF3000_DUCKDB}" >&2
  echo "run: SF=3000 MIN_DISK_GB=4000 bash scripts/setup_tpch_sf_db.sh" >&2
  exit 1
fi

psql_cmd() { psql -v ON_ERROR_STOP=1 "$@"; }

if [[ "${SKIP_CONTAINER:-0}" != "1" ]]; then
  echo "=== restarting ${NAME} with SF3000-sized shm/buffers ==="
  bash "${ROOT}/scripts/start_accel_pg_test.sh"
fi

if [[ "${SKIP_EXTENSION:-0}" != "1" ]]; then
  echo "=== building/installing accelerator_codegen_pg extension ==="
  bash "${ROOT}/scripts/build_accel_postgres_extension.sh"
fi

if [[ "${SKIP_LOAD:-0}" != "1" ]]; then
  echo "=== recreating database ${DB} ==="
  psql_cmd -d postgres -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '${DB}' AND pid <> pg_backend_pid();" >/dev/null || true
  psql_cmd -d postgres -c "DROP DATABASE IF EXISTS \"${DB}\";"
  psql_cmd -d postgres -c "CREATE DATABASE \"${DB}\";"
  psql_cmd -d "${DB}" -f "${ROOT}/scripts/postgres_tpch_schema.sql"

  echo "=== copying SF3000 from DuckDB into Postgres (parallel COPY) ==="
  echo "    source=${SF3000_DUCKDB}"
  # Cap workers: 80× DuckDB on a ~TB file gets SIGKILL (-9 / OOM).
  WORKERS="${ACCEL_PG_LOAD_WORKERS:-8}"
  echo "    ACCEL_PG_LOAD_WORKERS=${WORKERS}"
  "${PYTHON}" "${ROOT}/scripts/load_tpch_sf1_postgres.py" \
    --duckdb "${SF3000_DUCKDB}" \
    --db "${DB}" \
    --workers "${WORKERS}"

  if [[ "${SKIP_INDEXES:-0}" != "1" ]]; then
    echo "=== creating TPC-H indexes (expect many hours) ==="
    psql_cmd -d "${DB}" -f "${ROOT}/scripts/postgres_tpch_indexes.sql"
  fi
fi

EXTENSION_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so}"
echo "=== registering accelerator functions ==="
docker exec -i -e PGPASSWORD="${PGPASSWORD}" "${NAME}" \
  psql -v ON_ERROR_STOP=1 -U postgres -d "${DB}" <<SQL
LOAD '${EXTENSION_SO}';
\\i /accel-src/scripts/postgres_register_tpch_all.sql
SQL
if [[ -f "${ROOT}/scripts/postgres_accel_mutation_triggers.sql" ]]; then
  docker exec -i -e PGPASSWORD="${PGPASSWORD}" "${NAME}" \
    psql -v ON_ERROR_STOP=1 -U postgres -d "${DB}" \
    -f /accel-src/scripts/postgres_accel_mutation_triggers.sql || true
fi

LI="$(psql_cmd -d "${DB}" -tAc 'SELECT COUNT(*) FROM lineitem' | tr -d '[:space:]')"
echo "SF3000 Postgres ready: db=${DB} lineitem=${LI}"
echo "Next (aux vs native write-mix, lean queries only):"
echo "  ACCEL_PG_ALLOW_SMALL_DB=1 TPCH_PG_DB=${DB} OUT_DIR=benchmarks/tpch/results_pg_sf3000 \\"
echo "    QUERIES=1,5,6,7,8,9,10,11,12,14,15,16,17,18,19,21,22 \\"
echo "    bash scripts/run_tpch_pg_matview_vs_aux_write_mix_sweep.sh"
echo "(Prefer aux vs lean IMMV/MV columns; see docs/tpch_sf3000_aux_scale.md baseline pack.)"
